// pp_socket.cpp — Depth-1 ping-pong latency benchmark over Unix domain socket
//
// Uses socketpair(AF_UNIX, SOCK_STREAM) — created before fork so both ends
// share the same kernel buffer without a connect/accept round trip per run.
//
// Protocol:
//   Core A (initiator / parent):              Core B (echo / child):
//   t_start = pp_now_ns()
//   send(sock[0], payload, sz)  ──────────►   recv(sock[1], buf, sz)
//                                              send(sock[1], buf, sz)
//   recv(sock[0], buf, sz)      ◄──────────
//   t_end = pp_now_ns()
//   RTT = t_end - t_start
//
// Output: data/pingpong_socket_summary.csv

#include "common.h"
#include "clock_util.h"
#include "stats_util.h"

#include <fstream>
#include <iostream>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <cstring>

// ── Echo server (child, Core B) ───────────────────────────────────────────────
static void echo_server(int sock_fd, size_t msg_sz, size_t n_rounds) {
    pp_set_affinity(PP_ECHO_CORE);

    std::vector<char> buf(msg_sz);
    size_t total = PP_WARMUP + n_rounds;

    for (size_t i = 0; i < total; ++i) {
        if (!pp_read_all(sock_fd, buf.data(), msg_sz)) break;
        if (!pp_write_all(sock_fd, buf.data(), msg_sz)) break;
    }
    close(sock_fd);
    _exit(0);
}

// ── Initiator (parent, Core A) ────────────────────────────────────────────────
static PPStats run_initiator(int sock_fd, size_t msg_sz, size_t n_rounds) {
    std::vector<char>     payload(msg_sz, 0xCD);
    std::vector<uint64_t> rtts;
    rtts.reserve(n_rounds);

    size_t total = PP_WARMUP + n_rounds;

    for (size_t i = 0; i < total; ++i) {
        uint64_t t_start = pp_now_ns();

        if (!pp_write_all(sock_fd, payload.data(), msg_sz)) break;
        if (!pp_read_all (sock_fd, payload.data(), msg_sz)) break;

        uint64_t t_end = pp_now_ns();

        if (i >= PP_WARMUP)
            rtts.push_back(t_end - t_start);
    }

    return compute_pp_stats(rtts);
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main() {
    system("mkdir -p data");

    std::ofstream summary("data/pingpong_socket_summary.csv");
    summary << PP_SUMMARY_CSV_HEADER;

    std::cout << "[pp_socket] Starting depth-1 Unix socket ping-pong benchmark\n";
    std::cout << "  Mode: CLOCK_MONOTONIC_RAW, single clock source (Core "
              << PP_INITIATOR_CORE << ")\n\n";

    for (int si = 0; si < PP_NUM_SIZES; ++si) {
        size_t sz       = PP_MESSAGE_SIZES[si];
        size_t n_rounds = PP_ROUNDS[si];

        std::cout << "  size=" << sz << " B  rounds=" << n_rounds << " ... " << std::flush;

        // Create socket pair once per size, before fork
        int socks[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, socks) < 0) {
            std::perror("socketpair");
            return 1;
        }

        // Increase socket buffers to avoid partial-write stalls on large msgs
        int buf_sz = static_cast<int>(PP_MAX_PAYLOAD * 2);
        setsockopt(socks[0], SOL_SOCKET, SO_SNDBUF, &buf_sz, sizeof(buf_sz));
        setsockopt(socks[0], SOL_SOCKET, SO_RCVBUF, &buf_sz, sizeof(buf_sz));
        setsockopt(socks[1], SOL_SOCKET, SO_SNDBUF, &buf_sz, sizeof(buf_sz));
        setsockopt(socks[1], SOL_SOCKET, SO_RCVBUF, &buf_sz, sizeof(buf_sz));

        pid_t child = fork();
        if (child < 0) { std::perror("fork"); return 1; }

        if (child == 0) {
            // Child: use socks[1], close socks[0]
            close(socks[0]);
            echo_server(socks[1], sz, n_rounds);
            // never returns
        }

        // Parent: use socks[0], close socks[1]
        close(socks[1]);
        pp_set_affinity(PP_INITIATOR_CORE);

        PPStats s = run_initiator(socks[0], sz, n_rounds);

        close(socks[0]);
        int status = 0;
        waitpid(child, &status, 0);

        std::cout << "median=" << s.median_us
                  << " µs  p99=" << s.p99_us
                  << " µs  p99.9=" << s.p999_us << " µs\n";

        write_summary_row(summary, "unix_socket", "n/a", sz, s);
    }

    summary.close();
    std::cout << "\n[pp_socket] Done. Summary -> data/pingpong_socket_summary.csv\n";
    return 0;
}
