#ifndef COMMON_H
#define COMMON_H

#include <cstddef>
#include <cstdint>
#include <atomic>

// Core assignment architecture matrix
constexpr int    PRODUCER_CORE = 1;
constexpr int    CONSUMER_CORE = 2;
constexpr int    SQPOLL_CORE   = 3; 

constexpr size_t MESSAGE_SIZES[] = {
    64, 256, 1024, 4096, 16384, 65536, 262144, 1048576
};
constexpr int    NUM_RUNS        = 5;
constexpr size_t TOTAL_BYTES     = 2ULL * 1024 * 1024 * 1024; // 2GB
constexpr size_t NUM_SLOTS      = 64;
constexpr size_t MAX_PAYLOAD     = 1048576;                  // 1MB
constexpr size_t MAX_LAT_SAMPLES = 4 * 1024 * 1024;

constexpr char   SHM_RING_NAME[] = "/ipc_uring_ring_buffer";

struct alignas(64) RingBuffer {
    alignas(64) std::atomic<uint64_t> head;  
    alignas(64) std::atomic<uint64_t> tail;  

    struct alignas(64) Slot {
        uint64_t send_ns;          
        uint32_t size;
        char     data[MAX_PAYLOAD];
    };
    Slot slots[NUM_SLOTS];
};

struct Telemetry {
    double    latencies[MAX_LAT_SAMPLES];
    uint64_t latency_count;
    double    execution_time_sec;
};

#endif // COMMON_H
