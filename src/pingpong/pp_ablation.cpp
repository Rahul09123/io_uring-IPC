// pp_ablation.cpp — Depth-1 ping-pong over SHM with runtime-selectable wakeup
//
// Usage: ./pp_ablation <variant_int> [output_suffix]
//   variant_int: 0=busy_poll  1=spin_backoff  2=adaptive
//                3=futex      4=eventfd       5=io_uring
//
// Runs the full size sweep for the given wakeup variant and writes:
//   data/pingpong_ablation_<variant>_summary.csv
//
// This is the core result for the reviewer's request to "run the ping-pong
// protocol across all Part-A wakeup variants so latency, tails, and CPU cost
// are directly comparable."
//
// Channel structure: same PPChannel from pp_shm_uring.cpp (ready flag +
// reader_sleeping flag + data buffer). The wakeup mechanism is selected at
// runtime via switch.

#include "common.h"
#include "clock_util.h"
#include "stats_util.h"

#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <liburing.h>
#include <linux/futex.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

// ── Channel layout ────────────────────────────────────────────────────────────
struct alignas(64) PPChan {
    alignas(64) std::atomic<uint32_t> ready;           // 0=empty, 1=data ready
    alignas(64) std::atomic<uint32_t> reader_sleeping; // 0=awake, 1=sleeping
    alignas(64) uint32_t              data_size;
    char                              data[PP_MAX_PAYLOAD];
    // fd storage for eventfd/io_uring (set by consumer before loop)
    int  aux_fd   = -1;
};

// ── futex helpers ─────────────────────────────────────────────────────────────
static int futex_wait(uint32_t* addr, uint32_t val) {
    return static_cast<int>(
        syscall(SYS_futex, addr, FUTEX_WAIT,
                val, nullptr, nullptr, 0));
}
static int futex_wake(uint32_t* addr, int n) {
    return static_cast<int>(
        syscall(SYS_futex, addr, FUTEX_WAKE,
                n, nullptr, nullptr, 0));
}

// ── io_uring FIFO helpers ─────────────────────────────────────────────────────
static void uring_signal_pp(struct io_uring* ring, int fifo_fd) {
    static char sig = 'W';
    struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
    if (!sqe) return;
    io_uring_prep_write(sqe, fifo_fd, &sig, 1, 0);
    io_uring_submit(ring);
    struct io_uring_cqe* cqe;
    if (io_uring_wait_cqe(ring, &cqe) == 0) io_uring_cqe_seen(ring, cqe);
}
static void uring_wait_pp(struct io_uring* ring, int fifo_fd) {
    static char sb[8];
    struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
    if (!sqe) return;
    io_uring_prep_read(sqe, fifo_fd, sb, 1, 0);
    struct io_uring_cqe* cqe;
    if (io_uring_submit_and_wait(ring, 1) >= 0)
        if (io_uring_peek_cqe(ring, &cqe) == 0) io_uring_cqe_seen(ring, cqe);
}

// ── Wakeup variant enum ───────────────────────────────────────────────────────
enum Variant { BUSY_POLL=0, SPIN_BACKOFF=1, ADAPTIVE=2,
               FUTEX=3, EVENTFD=4, IO_URING=5 };

// ── Generic channel read — blocks until ready=1 ──────────────────────────────
static void chan_read(PPChan* ch, char* buf, size_t sz,
                      Variant v, struct io_uring* ring, int spin_threshold = 500) {
    int spins = 0;
    while (true) {
        if (ch->ready.load(std::memory_order_acquire) == 1) {
            std::memcpy(buf, ch->data, sz);
            ch->ready.store(0, std::memory_order_release);
            return;
        }

        switch (v) {
        case BUSY_POLL:
            continue;

        case SPIN_BACKOFF:
            for (int i = 0; i < 8; ++i) PP_PAUSE();
            continue;

        case ADAPTIVE:
            if (spins++ < spin_threshold) { PP_PAUSE(); continue; }
            spins = 0;
            // fall through to futex
            [[fallthrough]];

        case FUTEX: {
            ch->reader_sleeping.store(1, std::memory_order_seq_cst);
            if (ch->ready.load(std::memory_order_seq_cst) == 1) {
                uint32_t exp = 1;
                ch->reader_sleeping.compare_exchange_strong(
                    exp, 0, std::memory_order_seq_cst);
                continue;
            }
            futex_wait(reinterpret_cast<uint32_t*>(&ch->reader_sleeping), 1);
            continue;
        }

        case EVENTFD: {
            ch->reader_sleeping.store(1, std::memory_order_seq_cst);
            if (ch->ready.load(std::memory_order_seq_cst) == 1) {
                uint32_t exp = 1;
                ch->reader_sleeping.compare_exchange_strong(
                    exp, 0, std::memory_order_seq_cst);
                continue;
            }
            uint64_t val = 0;
            (void)read(ch->aux_fd, &val, sizeof(val));
            continue;
        }

        case IO_URING: {
            ch->reader_sleeping.store(1, std::memory_order_seq_cst);
            if (ch->ready.load(std::memory_order_seq_cst) == 1) {
                uint32_t exp = 1;
                ch->reader_sleeping.compare_exchange_strong(
                    exp, 0, std::memory_order_seq_cst);
                continue;
            }
            uring_wait_pp(ring, ch->aux_fd);
            continue;
        }
        }
    }
}

// ── Generic channel write — sets ready=1, signals if reader sleeping ──────────
static void chan_write(PPChan* ch, const char* payload, size_t sz,
                       Variant v, struct io_uring* ring) {
    std::memcpy(ch->data, payload, sz);
    ch->data_size = static_cast<uint32_t>(sz);
    ch->ready.store(1, std::memory_order_seq_cst);

    if (v == BUSY_POLL || v == SPIN_BACKOFF) return; // reader is spinning

    if (ch->reader_sleeping.load(std::memory_order_seq_cst) == 1) {
        uint32_t exp = 1;
        if (ch->reader_sleeping.compare_exchange_strong(
                exp, 0, std::memory_order_acq_rel)) {
            switch (v) {
            case ADAPTIVE:
            case FUTEX:
                futex_wake(reinterpret_cast<uint32_t*>(&ch->reader_sleeping), 1);
                break;
            case EVENTFD: {
                uint64_t val = 1;
                (void)write(ch->aux_fd, &val, sizeof(val));
                break;
            }
            case IO_URING:
                uring_signal_pp(ring, ch->aux_fd);
                break;
            default: break;
            }
        }
    }
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <variant_int 0-5>\n";
        return 1;
    }
    Variant variant = static_cast<Variant>(std::atoi(argv[1]));
    if (variant < 0 || variant > 5) {
        std::cerr << "variant must be 0-5\n"; return 1;
    }

    const char* vname = WAKEUP_VARIANT_NAMES[variant];
    system("mkdir -p data");

    std::string csv_path = std::string("data/pingpong_ablation_")
                           + vname + "_summary.csv";
    std::ofstream summary(csv_path);
    summary << PP_SUMMARY_CSV_HEADER;

    std::cout << "[pp_ablation:" << vname << "] Starting depth-1 SHM ping-pong\n";
    std::cout << "  Mode: CLOCK_MONOTONIC_RAW, single clock source (Core "
              << PP_INITIATOR_CORE << ")\n\n";

    for (int si = 0; si < PP_NUM_SIZES; ++si) {
        size_t sz       = PP_MESSAGE_SIZES[si];
        size_t n_rounds = PP_ROUNDS[si];

        std::cout << "  [" << vname << "] size=" << sz
                  << " B  rounds=" << n_rounds << " ... " << std::flush;

        // ── Create shared memory for two channels ─────────────────────────────
        const char* SHM_F = "/pp_abl_fwd";
        const char* SHM_B = "/pp_abl_bwd";
        shm_unlink(SHM_F); shm_unlink(SHM_B);
        int sfd_f = shm_open(SHM_F, O_CREAT|O_RDWR, 0666);
        int sfd_b = shm_open(SHM_B, O_CREAT|O_RDWR, 0666);
        ftruncate(sfd_f, sizeof(PPChan));
        ftruncate(sfd_b, sizeof(PPChan));
        auto* ch_fwd = static_cast<PPChan*>(
            mmap(nullptr, sizeof(PPChan), PROT_READ|PROT_WRITE, MAP_SHARED, sfd_f, 0));
        auto* ch_bwd = static_cast<PPChan*>(
            mmap(nullptr, sizeof(PPChan), PROT_READ|PROT_WRITE, MAP_SHARED, sfd_b, 0));
        ch_fwd->ready.store(0); ch_fwd->reader_sleeping.store(0); ch_fwd->aux_fd = -1;
        ch_bwd->ready.store(0); ch_bwd->reader_sleeping.store(0); ch_bwd->aux_fd = -1;

        // ── Variant-specific FD setup ─────────────────────────────────────────
        // For EVENTFD: create two eventfds (one per channel direction)
        // For IO_URING: create two FIFOs (one per channel direction)
        int efd_fwd = -1, efd_bwd = -1;
        const char* FIFO_F = "/tmp/pp_abl_fifo_fwd";
        const char* FIFO_B = "/tmp/pp_abl_fifo_bwd";
        int fifo_fwd = -1, fifo_bwd = -1;

        if (variant == EVENTFD) {
            efd_fwd = eventfd(0, EFD_SEMAPHORE);
            efd_bwd = eventfd(0, EFD_SEMAPHORE);
            // Store efd in channel aux_fd (via shared mem — it's a fd, not
            // inheritable across exec but valid after fork).
            // We pass fds via a pipe to child after fork:
            // Simpler: fork inherits open file descriptors.
            // We set aux_fd AFTER fork in each process.
        } else if (variant == IO_URING) {
            unlink(FIFO_F); unlink(FIFO_B);
            mkfifo(FIFO_F, 0666);
            mkfifo(FIFO_B, 0666);
        }

        pid_t child = fork();
        if (child < 0) { std::perror("fork"); return 1; }

        if (child == 0) {
            // ── Child: echo server on Core B ─────────────────────────────────
            pp_set_affinity(PP_ECHO_CORE);

            struct io_uring ring_rd{}, ring_wr{};
            if (variant == IO_URING) {
                io_uring_queue_init(64, &ring_rd, 0);
                io_uring_queue_init(64, &ring_wr, 0);
                fifo_fwd = open(FIFO_F, O_RDWR);
                fifo_bwd = open(FIFO_B, O_RDWR);
                ch_fwd->aux_fd = fifo_fwd; // used by chan_read for fwd channel
                ch_bwd->aux_fd = fifo_bwd; // used by chan_write for bwd channel
            } else if (variant == EVENTFD) {
                ch_fwd->aux_fd = efd_fwd;
                ch_bwd->aux_fd = efd_bwd;
            }

            std::vector<char> buf(sz);
            size_t total = PP_WARMUP + n_rounds;
            for (size_t i = 0; i < total; ++i) {
                chan_read(ch_fwd, buf.data(), sz, variant, &ring_rd);
                chan_write(ch_bwd, buf.data(), sz, variant, &ring_wr);
            }

            if (variant == IO_URING) {
                io_uring_queue_exit(&ring_rd);
                io_uring_queue_exit(&ring_wr);
            }
            _exit(0);
        }

        // ── Parent: initiator on Core A ───────────────────────────────────────
        pp_set_affinity(PP_INITIATOR_CORE);

        struct io_uring ring_wr{}, ring_rd{};
        if (variant == IO_URING) {
            io_uring_queue_init(64, &ring_wr, 0);
            io_uring_queue_init(64, &ring_rd, 0);
            fifo_fwd = open(FIFO_F, O_RDWR);
            fifo_bwd = open(FIFO_B, O_RDWR);
            ch_fwd->aux_fd = fifo_fwd;
            ch_bwd->aux_fd = fifo_bwd;
        } else if (variant == EVENTFD) {
            ch_fwd->aux_fd = efd_fwd;
            ch_bwd->aux_fd = efd_bwd;
        }

        std::vector<char>     payload(sz, 0xFF);
        std::vector<uint64_t> rtts;
        rtts.reserve(n_rounds);

        size_t total = PP_WARMUP + n_rounds;
        for (size_t i = 0; i < total; ++i) {
            uint64_t t_start = pp_now_ns();
            // Initiator writes fwd, reads bwd
            chan_write(ch_fwd, payload.data(), sz, variant, &ring_wr);
            chan_read(ch_bwd, payload.data(), sz, variant, &ring_rd);
            uint64_t t_end = pp_now_ns();
            if (i >= PP_WARMUP) rtts.push_back(t_end - t_start);
        }

        if (variant == IO_URING) {
            io_uring_queue_exit(&ring_wr);
            io_uring_queue_exit(&ring_rd);
        }

        int status = 0;
        waitpid(child, &status, 0);

        PPStats s = compute_pp_stats(rtts);

        // Cleanup
        munmap(ch_fwd, sizeof(PPChan));
        munmap(ch_bwd, sizeof(PPChan));
        close(sfd_f); close(sfd_b);
        shm_unlink(SHM_F); shm_unlink(SHM_B);
        if (efd_fwd >= 0) close(efd_fwd);
        if (efd_bwd >= 0) close(efd_bwd);
        if (fifo_fwd >= 0) close(fifo_fwd);
        if (fifo_bwd >= 0) close(fifo_bwd);
        if (variant == IO_URING) { unlink(FIFO_F); unlink(FIFO_B); }

        std::cout << "median=" << s.median_us
                  << " µs  p99=" << s.p99_us
                  << " µs  p99.9=" << s.p999_us << " µs\n";

        write_summary_row(summary, "shm_ablation", vname, sz, s);
    }

    summary.close();
    std::cout << "\n[pp_ablation:" << vname << "] Done. -> " << csv_path << "\n";
    return 0;
}
