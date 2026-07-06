#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sched.h>
#include <fcntl.h>
#include <iostream>
#include <cstring>
#include <chrono>
#include <vector>
#include <string>
#include "common.h"

static void set_affinity(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
}

static inline uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
}

static bool write_all(int fd, const void* buf, size_t n) {
    const char* p = static_cast<const char*>(buf);
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return false;
        p += w; n -= w;
    }
    return true;
}

static inline size_t get_total_bytes(size_t msg_sz) {
    if (msg_sz <= 1024)     return 32ULL * 1024 * 1024;  
    if (msg_sz <= 65536)    return 256ULL * 1024 * 1024; 
    return 2ULL * 1024 * 1024 * 1024;                    
}

int main() {
    set_affinity(PRODUCER_CORE);
    std::cout << "[Producer] Initializing Decoupled Pipe Benchmark Loops...\n";

    for (size_t sz : MESSAGE_SIZES) {
        std::cout << "[Producer] Processing Size Target: " << sz << " Bytes\n";
        std::string dynamic_fifo = std::string(PIPE_FIFO_PATH) + "_" + std::to_string(sz);

        std::vector<char> wire(sizeof(MessageHeader) + sz, 'X');
        auto* hdr = reinterpret_cast<MessageHeader*>(wire.data());
        hdr->payload_size = static_cast<uint32_t>(sz);

        size_t current_total_target = get_total_bytes(sz);

        // FIXED: Open the channel ONCE per payload sizing matrix, NOT inside the run loop
        int fd = -1;
        while (fd == -1) {
            fd = open(dynamic_fifo.c_str(), O_WRONLY);
            if (fd == -1) usleep(100); // Stall gracefully until consumer maps the FIFO node
        }

        fcntl(fd, F_SETPIPE_SZ, static_cast<int>(MAX_PAYLOAD));

        for (int run = 0; run <= NUM_RUNS; ++run) {
            size_t produced = 0;
            while (produced < current_total_target) {
                hdr->send_ns = now_ns();
                if (!write_all(fd, wire.data(), wire.size())) {
                    break;
                }
                produced += sz;
                sched_yield(); 
            }
        }
        close(fd);
        usleep(5000); 
    }
    std::cout << "[Producer] Production completed safely.\n";
    return 0;
}
