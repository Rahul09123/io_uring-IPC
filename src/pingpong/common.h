#ifndef PINGPONG_COMMON_H
#define PINGPONG_COMMON_H

#include <cstddef>
#include <cstdint>

// ── CPU affinity ──────────────────────────────────────────────────────────────
constexpr int PP_INITIATOR_CORE = 1; // Core A — owns all timestamps
constexpr int PP_ECHO_CORE      = 2; // Core B — echo server

// ── Round-trip counts ─────────────────────────────────────────────────────────
// Scaled by message size to keep wall time ≤ ~30s per (type × size)
constexpr size_t PP_WARMUP     = 10000; // discarded
// Measured rounds (indexed by MESSAGE_SIZES[] position)
constexpr size_t PP_ROUNDS[]   = {
    100000,  // 64 B
    100000,  // 256 B
    100000,  // 1 KB
     50000,  // 4 KB
     20000,  // 16 KB
     10000,  // 64 KB
      5000,  // 256 KB
      2000,  // 1 MB
};
constexpr size_t PP_MAX_ROUNDS = 100000; // upper bound for arrays

// ── Message sizes ─────────────────────────────────────────────────────────────
constexpr size_t PP_MESSAGE_SIZES[] = {
    64, 256, 1024, 4096, 16384, 65536, 262144, 1048576
};
constexpr int PP_NUM_SIZES = 8;

// ── SHM / FIFO names ─────────────────────────────────────────────────────────
constexpr char PP_PIPE_FWD[]  = "/tmp/pp_pipe_fwd";
constexpr char PP_PIPE_BWD[]  = "/tmp/pp_pipe_bwd";
constexpr char PP_SOCK_PATH[] = "/tmp/pp_unix_sock";
constexpr char PP_SHM_FWD[]   = "/pp_shm_fwd";
constexpr char PP_SHM_BWD[]   = "/pp_shm_bwd";
constexpr char PP_FIFO_FWD[]  = "/tmp/pp_uring_fifo_fwd";
constexpr char PP_FIFO_BWD[]  = "/tmp/pp_uring_fifo_bwd";
constexpr char PP_MQ_FWD[]    = "/pp_mq_fwd";
constexpr char PP_MQ_BWD[]    = "/pp_mq_bwd";

// ── Max payload ───────────────────────────────────────────────────────────────
constexpr size_t PP_MAX_PAYLOAD = 1048576; // 1 MB

// ── IPC type names (for CSV output) ──────────────────────────────────────────
constexpr const char* IPC_TYPE_NAMES[] = {
    "pipe", "unix_socket", "shm_io_uring", "posix_mq"
};
constexpr const char* WAKEUP_VARIANT_NAMES[] = {
    "busy_poll", "spin_backoff", "adaptive", "futex", "io_uring"
};

// ── CSV headers ───────────────────────────────────────────────────────────────
// Raw per-round output (optional, for detailed analysis)
constexpr char PP_RAW_CSV_HEADER[] =
    "ipc_type,wakeup_variant,message_size_bytes,round,rtt_ns\n";

// Summary output (one row per size)
constexpr char PP_SUMMARY_CSV_HEADER[] =
    "ipc_type,wakeup_variant,message_size_bytes,n_rounds,"
    "mean_us,median_us,p90_us,p99_us,p999_us,ci95_lo_us,ci95_hi_us\n";

#endif // PINGPONG_COMMON_H
