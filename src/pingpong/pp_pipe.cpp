// pp_pipe.cpp — Depth-1 ping-pong latency benchmark over POSIX named pipes
//
// Protocol (depth-1, single clock source):
//
//   Core A (initiator / parent):                Core B (echo / child):
//   t_start = pp_now_ns()
//   write_all(fwd_fd, payload, sz) ──────────►  read_all(fwd_fd, payload, sz)
//                                               write_all(bwd_fd, payload, sz)
//   read_all(bwd_fd,  payload, sz) ◄──────────
//   t_end = pp_now_ns()
//   RTT = t_end - t_start  [nanoseconds]
//
// BOTH timestamps are on Core A. No cross-core TSC subtraction. One-way
// latency ≈ RTT / 2.
//
// Output: data/pingpong_pipe_summary.csv

#include "common.h"
#include "clock_util.h"
#include "stats_util.h"

#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <cstring>

// ── Echo server (runs in child, Core B) ──────────────────────────────────────
static void echo_server(size_t msg_sz, size_t total_rounds) {
    pp_set_affinity(PP_ECHO_CORE);

    int fwd_fd = open(PP_PIPE_FWD, O_RDONLY);
    int bwd_fd = open(PP_PIPE_BWD, O_WRONLY);
    if (fwd_fd < 0 || bwd_fd < 0) {
        std::perror("[echo] open pipes");
        _exit(1);
    }

    std::vector<char> buf(msg_sz);
    size_t rounds = PP_WARMUP + total_rounds;

    for (size_t i = 0; i < rounds; ++i) {
        if (!pp_read_all(fwd_fd, buf.data(), msg_sz)) break;
        if (!pp_write_all(bwd_fd, buf.data(), msg_sz)) break;
    }

    close(fwd_fd);
    close(bwd_fd);
    _exit(0);
}

// ── Initiator (runs in parent, Core A) ───────────────────────────────────────
static PPStats run_initiator(int fwd_fd, int bwd_fd,
                              size_t msg_sz, size_t n_rounds) {
    std::vector<char>     payload(msg_sz, 0xAB);
    std::vector<uint64_t> rtts;
    rtts.reserve(n_rounds);

    size_t total = PP_WARMUP + n_rounds;

    for (size_t i = 0; i < total; ++i) {
        uint64_t t_start = pp_now_ns();

        if (!pp_write_all(fwd_fd, payload.data(), msg_sz)) break;
        if (!pp_read_all (bwd_fd, payload.data(), msg_sz)) break;

        uint64_t t_end = pp_now_ns();

        if (i >= PP_WARMUP)
            rtts.push_back(t_end - t_start);
    }

    return compute_pp_stats(rtts);
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main() {
    // Ensure output directory exists
    system("mkdir -p data");

    std::ofstream summary("data/pingpong_pipe_summary.csv");
    summary << PP_SUMMARY_CSV_HEADER;

    std::cout << "[pp_pipe] Starting depth-1 pipe ping-pong benchmark\n";
    std::cout << "  Mode: CLOCK_MONOTONIC_RAW, single clock source (Core "
              << PP_INITIATOR_CORE << ")\n\n";

    for (int si = 0; si < PP_NUM_SIZES; ++si) {
        size_t sz       = PP_MESSAGE_SIZES[si];
        size_t n_rounds = PP_ROUNDS[si];

        std::cout << "  size=" << sz << " B  rounds=" << n_rounds << " ... " << std::flush;

        // Create FIFOs fresh for each size
        unlink(PP_PIPE_FWD);
        unlink(PP_PIPE_BWD);
        if (mkfifo(PP_PIPE_FWD, 0666) || mkfifo(PP_PIPE_BWD, 0666)) {
            std::perror("mkfifo");
            return 1;
        }

        pid_t child = fork();
        if (child < 0) { std::perror("fork"); return 1; }

        if (child == 0) {
            // Child: echo server
            echo_server(sz, n_rounds);
            // never returns
        }

        // Parent: initiator on Core A
        pp_set_affinity(PP_INITIATOR_CORE);

        // Open fwd for writing (blocks until echo opens for reading)
        int fwd_fd = open(PP_PIPE_FWD, O_WRONLY);
        // Open bwd for reading (blocks until echo opens for writing)
        int bwd_fd = open(PP_PIPE_BWD, O_RDONLY);

        if (fwd_fd < 0 || bwd_fd < 0) {
            std::perror("open pipes (initiator)");
            kill(child, SIGKILL);
            return 1;
        }

        // Increase pipe buffer to avoid partial-write stalls
        fcntl(fwd_fd, F_SETPIPE_SZ, static_cast<int>(PP_MAX_PAYLOAD));
        fcntl(bwd_fd, F_SETPIPE_SZ, static_cast<int>(PP_MAX_PAYLOAD));

        PPStats s = run_initiator(fwd_fd, bwd_fd, sz, n_rounds);

        close(fwd_fd);
        close(bwd_fd);
        int status = 0;
        waitpid(child, &status, 0);

        unlink(PP_PIPE_FWD);
        unlink(PP_PIPE_BWD);

        std::cout << "median=" << s.median_us
                  << " µs  p99=" << s.p99_us
                  << " µs  p99.9=" << s.p999_us << " µs\n";

        write_summary_row(summary, "pipe", "n/a", sz, s);
    }

    summary.close();
    std::cout << "\n[pp_pipe] Done. Summary -> data/pingpong_pipe_summary.csv\n";
    return 0;
}
