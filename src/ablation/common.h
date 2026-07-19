#ifndef ABLATION_COMMON_H
#define ABLATION_COMMON_H

#include <cstddef>
#include <cstdint>

// ── CPU affinity ──────────────────────────────────────────────────────────────
constexpr int PRODUCER_CORE = 1;
constexpr int CONSUMER_CORE = 2;

// ── Benchmark parameters ──────────────────────────────────────────────────────
constexpr size_t MESSAGE_SIZES[] = {64, 256, 1024, 4096,
                                    16384, 65536, 262144, 1048576};
constexpr int    NUM_RUNS        = 15;
static inline size_t get_total_bytes(size_t msg_sz) {
    if (msg_sz <= 1024)     return 16ULL * 1024 * 1024;  // 16 MB (fast sweep for small messages)
    if (msg_sz <= 65536)    return 128ULL * 1024 * 1024; // 128 MB
    return 512ULL * 1024 * 1024;                        // 512 MB for large messages
}
constexpr size_t NUM_SLOTS       = 64;
constexpr size_t MAX_PAYLOAD     = 1048576;               // 1 MB
constexpr size_t MAX_LAT_SAMPLES = 4 * 1024 * 1024;

// ── Shared-memory / FIFO names ────────────────────────────────────────────────
constexpr char SHM_RING_NAME[]      = "/ipc_ablation_ring";
constexpr char SIGNAL_PATH[]        = "/tmp/ablation_sig_fifo";
constexpr char EVENTFD_SOCKET_PATH[] = "/tmp/ablation_efd_socket";

#include <sys/socket.h>
#include <sys/un.h>
#include <cstring>
#include <unistd.h>

static inline int send_fd(int sock, int fd) {
    struct msghdr msg = {0};
    char buf[CMSG_SPACE(sizeof(int))] = {0};
    
    struct iovec io = {
        .iov_base = const_cast<char*>("F"),
        .iov_len = 1
    };
    
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = buf;
    msg.msg_controllen = sizeof(buf);
    
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    
    std::memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
    
    return static_cast<int>(sendmsg(sock, &msg, 0));
}

static inline int recv_fd(int sock) {
    struct msghdr msg = {0};
    char ctrl_buf[CMSG_SPACE(sizeof(int))];
    char dummy_buf[64];
    
    struct iovec io = {
        .iov_base = dummy_buf,
        .iov_len = sizeof(dummy_buf)
    };
    
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl_buf;
    msg.msg_controllen = sizeof(ctrl_buf);
    
    if (recvmsg(sock, &msg, 0) < 0) return -1;
    
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_type != SCM_RIGHTS) return -1;
    
    int fd = -1;
    std::memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
    return fd;
}

// ── Adaptive-spin threshold ───────────────────────────────────────────────────
constexpr int ADAPTIVE_SPIN_ITERS = 500;

// ── Bursty inter-arrival (µs) for offered-load sweep ─────────────────────────
//   These create 25 / 50 / 75 / 90% of saturated throughput on a typical
//   server core.  Adjust per-machine if needed.
constexpr int INTER_ARRIVAL_US_25 = 1200;
constexpr int INTER_ARRIVAL_US_50 =  600;
constexpr int INTER_ARRIVAL_US_75 =  200;
constexpr int INTER_ARRIVAL_US_90 =   50;

// ── Wakeup variants ───────────────────────────────────────────────────────────
enum WakeupVariant : int {
    BUSY_POLL    = 0,
    SPIN_BACKOFF = 1,
    ADAPTIVE     = 2,
    FUTEX        = 3,
    EVENTFD      = 4,
    IO_URING     = 5,
};
constexpr const char* VARIANT_NAMES[] = {
    "busy_poll", "spin_backoff", "adaptive", "futex", "eventfd", "io_uring"
};
constexpr int NUM_VARIANTS = 6;

// ── Arrival regimes ───────────────────────────────────────────────────────────
enum Regime : int {
    SATURATED    = 0,
    BURSTY       = 1,   // one message, then sleep
    OFFERED_25   = 2,
    OFFERED_50   = 3,
    OFFERED_75   = 4,
    OFFERED_90   = 5,
};
constexpr const char* REGIME_NAMES[] = {
    "saturated", "bursty", "offered_25", "offered_50", "offered_75", "offered_90"
};
constexpr int NUM_REGIMES = 6;

// ── CSV column header ─────────────────────────────────────────────────────────
constexpr char CSV_HEADER[] =
    "wakeup_variant,regime,message_size_bytes,run,"
    "throughput_gbps,avg_latency_us,stddev_us,p50_us,p95_us,p99_us,p999_us,"
    "wakeup_latency_p50_us,wakeup_latency_p99_us,"
    "wakeups_triggered,cpu_us_per_msg\n";

#endif // ABLATION_COMMON_H
