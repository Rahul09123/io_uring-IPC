// pp_shm_uring.cpp — Depth-1 ping-pong over shared-memory + io_uring wakeup
//
// Two shared-memory "channels" (one per direction).  Each channel is a simple
// structure with a data buffer + a ready flag.  The io_uring / FIFO mechanism
// (identical to src/io_uring/ design) is used as the wakeup.
//
// Design:
//   Forward  channel (PPChannel):  initiator writes → echo reads
//   Backward channel (PPChannel):  echo    writes → initiator reads
//
//   Each channel has:
//     • atomic<uint32_t> ready       — 0=empty, 1=data_ready
//     • atomic<uint32_t> reader_sleeping
//     • char             data[PP_MAX_PAYLOAD]
//
//   Wakeup path (same as src/io_uring/ implementation):
//     Reader: sets reader_sleeping=1, double-checks ready,
//             then blocks via io_uring_submit_and_wait on FIFO read.
//     Writer: if reader_sleeping==1, CAS to 0, writes 'W' to FIFO
//             via io_uring_prep_write.
//
// Output: data/pingpong_shm_uring_summary.csv

#include "common.h"
#include "clock_util.h"
#include "stats_util.h"

#include <atomic>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <liburing.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <cstring>

// ── Channel layout ────────────────────────────────────────────────────────────
struct alignas(64) PPChannel {
    alignas(64) std::atomic<uint32_t> ready;           // 0=empty, 1=data ready
    alignas(64) std::atomic<uint32_t> reader_sleeping; // wakeup flag
    alignas(64) uint32_t              data_size;
    char                              data[PP_MAX_PAYLOAD];
};

struct PPShm {
    PPChannel fwd; // initiator → echo
    PPChannel bwd; // echo      → initiator
};

// ── io_uring wakeup helpers ───────────────────────────────────────────────────
static void uring_signal(struct io_uring* ring, int fifo_fd) {
    static char sig = 'W';
    struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
    if (!sqe) return;
    io_uring_prep_write(sqe, fifo_fd, &sig, 1, 0);
    io_uring_submit(ring);
    struct io_uring_cqe* cqe;
    if (io_uring_wait_cqe(ring, &cqe) == 0)
        io_uring_cqe_seen(ring, cqe);
}

static void uring_wait(struct io_uring* ring, int fifo_fd) {
    static char sig_buf[8];
    struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
    if (!sqe) return;
    io_uring_prep_read(sqe, fifo_fd, sig_buf, 1, 0);
    struct io_uring_cqe* cqe;
    if (io_uring_submit_and_wait(ring, 1) >= 0) {
        if (io_uring_peek_cqe(ring, &cqe) == 0)
            io_uring_cqe_seen(ring, cqe);
    }
}

// ── Channel write (writer path) ───────────────────────────────────────────────
static void channel_write(PPChannel* ch, const char* payload, size_t sz,
                           struct io_uring* ring, int fifo_fd) {
    std::memcpy(ch->data, payload, sz);
    ch->data_size = static_cast<uint32_t>(sz);
    // Publish: set ready=1 with seq_cst to prevent store reordering
    ch->ready.store(1, std::memory_order_seq_cst);

    // Wake reader if sleeping
    if (ch->reader_sleeping.load(std::memory_order_seq_cst) == 1) {
        uint32_t expected = 1;
        if (ch->reader_sleeping.compare_exchange_strong(
                expected, 0, std::memory_order_acq_rel)) {
            uring_signal(ring, fifo_fd);
        }
    }
}

// ── Channel read (reader path) ────────────────────────────────────────────────
static void channel_read(PPChannel* ch, char* buf,
                          struct io_uring* ring, int fifo_fd) {
    while (true) {
        if (ch->ready.load(std::memory_order_acquire) == 1) {
            std::memcpy(buf, ch->data, ch->data_size);
            ch->ready.store(0, std::memory_order_release);
            return;
        }
        // Prepare to sleep
        ch->reader_sleeping.store(1, std::memory_order_seq_cst);
        // Double-check to avoid lost wakeup
        if (ch->ready.load(std::memory_order_seq_cst) == 1) {
            uint32_t exp = 1;
            ch->reader_sleeping.compare_exchange_strong(
                exp, 0, std::memory_order_seq_cst);
            continue;
        }
        uring_wait(ring, fifo_fd);
    }
}

// ── Echo server (child, Core B) ───────────────────────────────────────────────
static void echo_server(PPShm* shm, int fifo_fwd_fd, int fifo_bwd_fd,
                          size_t msg_sz, size_t n_rounds) {
    pp_set_affinity(PP_ECHO_CORE);

    struct io_uring ring_rd{}, ring_wr{};
    io_uring_queue_init(64, &ring_rd, 0);
    io_uring_queue_init(64, &ring_wr, 0);

    std::vector<char> buf(msg_sz);
    size_t total = PP_WARMUP + n_rounds;

    for (size_t i = 0; i < total; ++i) {
        // Read from forward channel
        channel_read(&shm->fwd, buf.data(), &ring_rd, fifo_fwd_fd);
        // Echo back on backward channel
        channel_write(&shm->bwd, buf.data(), msg_sz, &ring_wr, fifo_bwd_fd);
    }

    io_uring_queue_exit(&ring_rd);
    io_uring_queue_exit(&ring_wr);
    _exit(0);
}

// ── Initiator (parent, Core A) ────────────────────────────────────────────────
static PPStats run_initiator(PPShm* shm, int fifo_fwd_fd, int fifo_bwd_fd,
                               size_t msg_sz, size_t n_rounds) {
    std::vector<char>     payload(msg_sz, 0xEF);
    std::vector<uint64_t> rtts;
    rtts.reserve(n_rounds);

    struct io_uring ring_wr{}, ring_rd{};
    io_uring_queue_init(64, &ring_wr, 0);
    io_uring_queue_init(64, &ring_rd, 0);

    size_t total = PP_WARMUP + n_rounds;

    for (size_t i = 0; i < total; ++i) {
        uint64_t t_start = pp_now_ns();

        // Send to echo on forward channel
        channel_write(&shm->fwd, payload.data(), msg_sz, &ring_wr, fifo_fwd_fd);
        // Wait for echo on backward channel
        channel_read(&shm->bwd, payload.data(), &ring_rd, fifo_bwd_fd);

        uint64_t t_end = pp_now_ns();

        if (i >= PP_WARMUP)
            rtts.push_back(t_end - t_start);
    }

    io_uring_queue_exit(&ring_wr);
    io_uring_queue_exit(&ring_rd);

    return compute_pp_stats(rtts);
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main() {
    system("mkdir -p data");

    std::ofstream summary("data/pingpong_shm_uring_summary.csv");
    summary << PP_SUMMARY_CSV_HEADER;

    std::cout << "[pp_shm_uring] Starting depth-1 SHM+io_uring ping-pong benchmark\n";
    std::cout << "  Mode: CLOCK_MONOTONIC_RAW, single clock source (Core "
              << PP_INITIATOR_CORE << ")\n\n";

    // Create FIFOs (two: one per direction)
    unlink(PP_FIFO_FWD); unlink(PP_FIFO_BWD);
    mkfifo(PP_FIFO_FWD, 0666);
    mkfifo(PP_FIFO_BWD, 0666);

    for (int si = 0; si < PP_NUM_SIZES; ++si) {
        size_t sz       = PP_MESSAGE_SIZES[si];
        size_t n_rounds = PP_ROUNDS[si];

        std::cout << "  size=" << sz << " B  rounds=" << n_rounds << " ... " << std::flush;

        // Create fresh SHM each size
        shm_unlink(PP_SHM_FWD); shm_unlink(PP_SHM_BWD);
        int shm_fwd_fd = shm_open(PP_SHM_FWD, O_CREAT | O_RDWR, 0666);
        int shm_bwd_fd = shm_open(PP_SHM_BWD, O_CREAT | O_RDWR, 0666);
        ftruncate(shm_fwd_fd, sizeof(PPChannel));
        ftruncate(shm_bwd_fd, sizeof(PPChannel));

        auto* ch_fwd = static_cast<PPChannel*>(
            mmap(nullptr, sizeof(PPChannel), PROT_READ|PROT_WRITE, MAP_SHARED, shm_fwd_fd, 0));
        auto* ch_bwd = static_cast<PPChannel*>(
            mmap(nullptr, sizeof(PPChannel), PROT_READ|PROT_WRITE, MAP_SHARED, shm_bwd_fd, 0));

        // Initialise channels
        ch_fwd->ready.store(0); ch_fwd->reader_sleeping.store(0);
        ch_bwd->ready.store(0); ch_bwd->reader_sleeping.store(0);

        // Combine into one PPShm view (we pass pointers separately)
        // Lay out fwd/bwd as separate mmaps — simpler than one big struct

        // Open FIFOs (both sides of each FIFO must be open O_RDWR to avoid
        // blocking on open when no reader/writer is present yet)
        int fifo_fwd = open(PP_FIFO_FWD, O_RDWR);
        int fifo_bwd = open(PP_FIFO_BWD, O_RDWR);

        pid_t child = fork();
        if (child < 0) { std::perror("fork"); return 1; }

        if (child == 0) {
            // Child echo server: reads fwd, writes bwd
            struct PPShm shm_child{};
            // Map the same SHMs in child
            (void)shm_child; // not used directly, we use ch_fwd/ch_bwd from parent mapping
            // After fork, child inherits parent's mmap — no re-mapping needed
            pp_set_affinity(PP_ECHO_CORE);

            struct io_uring ring_rd{}, ring_wr{};
            io_uring_queue_init(64, &ring_rd, 0);
            io_uring_queue_init(64, &ring_wr, 0);
            std::vector<char> buf(sz);
            size_t total = PP_WARMUP + n_rounds;
            for (size_t i = 0; i < total; ++i) {
                channel_read(ch_fwd, buf.data(), &ring_rd, fifo_fwd);
                channel_write(ch_bwd, buf.data(), sz, &ring_wr, fifo_bwd);
            }
            io_uring_queue_exit(&ring_rd);
            io_uring_queue_exit(&ring_wr);
            _exit(0);
        }

        // Parent: initiator on Core A
        pp_set_affinity(PP_INITIATOR_CORE);

        struct io_uring ring_wr{}, ring_rd{};
        io_uring_queue_init(64, &ring_wr, 0);
        io_uring_queue_init(64, &ring_rd, 0);

        std::vector<char>     payload(sz, 0xEF);
        std::vector<uint64_t> rtts;
        rtts.reserve(n_rounds);

        size_t total = PP_WARMUP + n_rounds;
        for (size_t i = 0; i < total; ++i) {
            uint64_t t_start = pp_now_ns();
            channel_write(ch_fwd, payload.data(), sz, &ring_wr, fifo_fwd);
            channel_read(ch_bwd, payload.data(), &ring_rd, fifo_bwd);
            uint64_t t_end = pp_now_ns();
            if (i >= PP_WARMUP) rtts.push_back(t_end - t_start);
        }

        io_uring_queue_exit(&ring_wr);
        io_uring_queue_exit(&ring_rd);

        int status = 0;
        waitpid(child, &status, 0);

        PPStats s = compute_pp_stats(rtts);
        munmap(ch_fwd, sizeof(PPChannel));
        munmap(ch_bwd, sizeof(PPChannel));
        close(shm_fwd_fd); close(shm_bwd_fd);
        close(fifo_fwd);   close(fifo_bwd);
        shm_unlink(PP_SHM_FWD); shm_unlink(PP_SHM_BWD);

        std::cout << "median=" << s.median_us
                  << " µs  p99=" << s.p99_us
                  << " µs  p99.9=" << s.p999_us << " µs\n";

        write_summary_row(summary, "shm_io_uring", "io_uring", sz, s);
    }

    unlink(PP_FIFO_FWD); unlink(PP_FIFO_BWD);
    summary.close();
    std::cout << "\n[pp_shm_uring] Done. Summary -> data/pingpong_shm_uring_summary.csv\n";
    return 0;
}
