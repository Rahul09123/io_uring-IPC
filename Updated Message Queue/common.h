#ifndef COMMON_H
#define COMMON_H

#include <cstddef>
#include <cstdint>

constexpr int    PRODUCER_CORE = 1;
constexpr int    CONSUMER_CORE = 2;

constexpr size_t MESSAGE_SIZES[] = {
    64, 256, 1024, 4096, 16384, 65536, 262144, 1048576
};

constexpr int    NUM_RUNS        = 15; // Set to 15
constexpr size_t MAX_PAYLOAD     = 1048576;
constexpr size_t MAX_LAT_SAMPLES = 4 * 1024 * 1024;
constexpr char   SHM_TEL_NAME[]   = "/ipc_mq_telemetry"; // Added missing macro

struct MessageHeader {
    uint64_t send_ns;       
    uint32_t payload_size;
};

struct Telemetry {
    double    latencies[MAX_LAT_SAMPLES];
    uint64_t latency_count;
    double    execution_time_sec;
};

static inline size_t get_total_bytes(size_t msg_sz) {
    if (msg_sz <= 1024)     return 32ULL * 1024 * 1024;  
    if (msg_sz <= 65536)    return 256ULL * 1024 * 1024; 
    return 2ULL * 1024 * 1024 * 1024;                    
}

#endif // COMMON_H
