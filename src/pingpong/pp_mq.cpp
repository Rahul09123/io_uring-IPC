// pp_mq.cpp — Depth-1 ping-pong latency benchmark over POSIX message queues
//
// Two queues per run:
//   /pp_mq_fwd  — initiator → echo
//   /pp_mq_bwd  — echo      → initiator
//
// Note: POSIX MQ has a maximum message size enforced at queue creation.
// We re-create both queues for each message size with mq_msgsize = msg_sz.
// mq_maxmsg = 1 (depth-1: no buffering needed).
//
// Output: data/pingpong_mq_summary.csv

#include "common.h"
#include "clock_util.h"
#include "stats_util.h"

#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <mqueue.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <cstring>

// ── Echo server (child, Core B) ───────────────────────────────────────────────
static void echo_server(const char* fwd_name, const char* bwd_name,
                          size_t msg_sz, size_t n_rounds) {
    pp_set_affinity(PP_ECHO_CORE);

    // Open queues (created by initiator before fork)
    mqd_t mq_fwd = mq_open(fwd_name, O_RDONLY);
    mqd_t mq_bwd = mq_open(bwd_name, O_WRONLY);
    if (mq_fwd == (mqd_t)-1 || mq_bwd == (mqd_t)-1) {
        std::perror("[echo] mq_open");
        _exit(1);
    }

    std::vector<char> buf(msg_sz + 1);
    size_t total = PP_WARMUP + n_rounds;

    for (size_t i = 0; i < total; ++i) {
        unsigned int prio = 0;
        ssize_t r = mq_receive(mq_fwd, buf.data(),
                               static_cast<size_t>(msg_sz + 1), &prio);
        if (r < 0) break;
        if (mq_send(mq_bwd, buf.data(), static_cast<size_t>(r), 0) < 0) break;
    }

    mq_close(mq_fwd);
    mq_close(mq_bwd);
    _exit(0);
}

// ── Initiator (parent, Core A) ────────────────────────────────────────────────
static PPStats run_initiator(mqd_t mq_fwd, mqd_t mq_bwd,
                               size_t msg_sz, size_t n_rounds) {
    std::vector<char>     payload(msg_sz, 0xBA);
    std::vector<uint64_t> rtts;
    rtts.reserve(n_rounds);

    size_t total = PP_WARMUP + n_rounds;

    for (size_t i = 0; i < total; ++i) {
        uint64_t t_start = pp_now_ns();

        if (mq_send(mq_fwd, payload.data(), msg_sz, 0) < 0) break;

        unsigned int prio = 0;
        ssize_t r = mq_receive(mq_bwd, payload.data(),
                               msg_sz, &prio);
        if (r < 0) break;

        uint64_t t_end = pp_now_ns();

        if (i >= PP_WARMUP)
            rtts.push_back(t_end - t_start);
    }

    return compute_pp_stats(rtts);
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main() {
    system("mkdir -p data");

    std::ofstream summary("data/pingpong_mq_summary.csv");
    summary << PP_SUMMARY_CSV_HEADER;

    std::cout << "[pp_mq] Starting depth-1 POSIX MQ ping-pong benchmark\n";
    std::cout << "  Mode: CLOCK_MONOTONIC_RAW, single clock source (Core "
              << PP_INITIATOR_CORE << ")\n\n";

    for (int si = 0; si < PP_NUM_SIZES; ++si) {
        size_t sz       = PP_MESSAGE_SIZES[si];
        size_t n_rounds = PP_ROUNDS[si];

        std::cout << "  size=" << sz << " B  rounds=" << n_rounds << " ... " << std::flush;

        // Remove stale queues
        mq_unlink(PP_MQ_FWD);
        mq_unlink(PP_MQ_BWD);

        // Create queues with mq_msgsize = sz, mq_maxmsg = 4
        // (mq_maxmsg must be ≥ 1; use 4 to absorb any momentary pipelining)
        struct mq_attr attr{};
        attr.mq_flags   = 0;
        attr.mq_maxmsg  = 4;
        attr.mq_msgsize = static_cast<long>(sz);

        mqd_t mq_fwd = mq_open(PP_MQ_FWD, O_CREAT | O_RDWR, 0666, &attr);
        mqd_t mq_bwd = mq_open(PP_MQ_BWD, O_CREAT | O_RDWR, 0666, &attr);

        if (mq_fwd == (mqd_t)-1 || mq_bwd == (mqd_t)-1) {
            std::perror("mq_open create");
            // POSIX MQ max message size may be limited by /proc/sys/fs/mqueue/msgsize_max
            std::cerr << "  HINT: sudo sysctl -w fs.mqueue.msgsize_max="
                      << (sz * 2) << "\n";
            write_summary_row(summary, "posix_mq", "n/a", sz, PPStats{});
            continue;
        }

        pid_t child = fork();
        if (child < 0) { std::perror("fork"); return 1; }

        if (child == 0) {
            mq_close(mq_fwd);
            mq_close(mq_bwd);
            echo_server(PP_MQ_FWD, PP_MQ_BWD, sz, n_rounds);
            // never returns
        }

        // Parent: use read end of fwd (send to echo) and write end of bwd (recv from echo)
        // Re-open with appropriate flags
        // mq_fwd is opened O_WRONLY for sending, mq_bwd O_RDONLY for receiving
        mq_close(mq_fwd);
        mq_close(mq_bwd);

        mqd_t mq_send_fd  = mq_open(PP_MQ_FWD, O_WRONLY);
        mqd_t mq_recv_fd  = mq_open(PP_MQ_BWD, O_RDONLY);

        pp_set_affinity(PP_INITIATOR_CORE);
        PPStats s = run_initiator(mq_send_fd, mq_recv_fd, sz, n_rounds);

        mq_close(mq_send_fd);
        mq_close(mq_recv_fd);

        int status = 0;
        waitpid(child, &status, 0);

        mq_unlink(PP_MQ_FWD);
        mq_unlink(PP_MQ_BWD);

        std::cout << "median=" << s.median_us
                  << " µs  p99=" << s.p99_us
                  << " µs  p99.9=" << s.p999_us << " µs\n";

        write_summary_row(summary, "posix_mq", "n/a", sz, s);
    }

    summary.close();
    std::cout << "\n[pp_mq] Done. Summary -> data/pingpong_mq_summary.csv\n";
    return 0;
}
