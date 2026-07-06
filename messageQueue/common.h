#ifndef COMMON_H
#define COMMON_H

#include <cstddef>
#include <cstdint>

constexpr int    PRODUCER_CORE = 1;
constexpr int    CONSUMER_CORE = 2;

constexpr size_t MESSAGE_SIZES[] = {
    64, 256, 1024, 4096, 16384, 65536, 262144, 1048576
};
constexpr int    NUM_RUNS        = 5;
constexpr size_t MAX_PAYLOAD     = 1048576;                  // 1MB
constexpr size_t MAX_LAT_SAMPLES = 4 * 1024 * 1024;
constexpr char   MQ_NAME[]       = "/p03_benchmark_mq";

// DYNAMIC WORKLOAD MATRIX: Prevents CPU thrashing on microscopic payload blocks
static inline size_t get_total_bytes(size_t msg_sz) {
    if (msg_sz <= 1024)     return 32ULL * 1024 * 1024;  // 32MB for small sizes
    if (msg_sz <= 65536)    return 256ULL * 1024 * 1024; // 256MB for medium sizes
    return 2ULL * 1024 * 1024 * 1024;                    // Full 2GB for large blocks
}

struct MessageHeader {
    uint64_t send_ns;       
    uint32_t payload_size;
};

struct Telemetry {
    double    latencies[MAX_LAT_SAMPLES];
    uint64_t latency_count;
    double    execution_time_sec;
};

#endif // COMMON_H
