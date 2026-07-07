#include <fcntl.h>
#include <sys/stat.h>
#include <mqueue.h>
#include <unistd.h>
#include <sched.h>
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

int main() {
    set_affinity(PRODUCER_CORE);
    std::cout << "[Producer] Initializing POSIX Message Queue Benchmark Loops...\n";

    for (size_t sz : MESSAGE_SIZES) {
        std::cout << "[Producer] Processing Size Target: " << sz << " Bytes\n";
        
        // FIXED: Dynamically map names per frame size matrix boundary
        std::string unique_mq_name = "/ipc_mq_bench_" + std::to_string(sz);

        std::vector<char> wire(sizeof(MessageHeader) + sz, 'X');
        auto* hdr = reinterpret_cast<MessageHeader*>(wire.data());
        hdr->payload_size = static_cast<uint32_t>(sz);

        size_t current_total_target = get_total_bytes(sz);

        mqd_t mq = -1;
        while (mq == (mqd_t)-1) {
            mq = mq_open(unique_mq_name.c_str(), O_WRONLY);
            if (mq == (mqd_t)-1) usleep(100); 
        }

        for (int run = 0; run <= NUM_RUNS; ++run) {
            size_t produced = 0;
            while (produced < current_total_target) {
                hdr->send_ns = now_ns();
                
                if (mq_send(mq, wire.data(), wire.size(), 0) == -1) {
                    std::perror("[Producer] mq_send failed");
                    break;
                }
                produced += sz;
            }
            usleep(2000);
        }
        mq_close(mq);
        usleep(5000);
    }
    std::cout << "[Producer] Run complete.\n";
    return 0;
}
