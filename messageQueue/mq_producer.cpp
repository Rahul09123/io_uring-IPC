#include <sys/stat.h>
#include <mqueue.h>
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

int main() {
    set_affinity(PRODUCER_CORE);
    std::cout << "[Producer] Initializing POSIX Message Queue Benchmark Loops...\n";

    for (size_t sz : MESSAGE_SIZES) {
        std::cout << "[Producer] Processing Size Target: " << sz << " Bytes\n";

        // Generate unique name per payload iteration step to break race conditions
        std::string dynamic_name = std::string(MQ_NAME) + "_" + std::to_string(sz);

        struct mq_attr attr;
        attr.mq_flags = 0;
        attr.mq_maxmsg = 5; 
        attr.mq_msgsize = sizeof(MessageHeader) + sz; 
        attr.mq_curmsgs = 0;

        mq_unlink(dynamic_name.c_str());
        mqd_t mq = mq_open(dynamic_name.c_str(), O_CREAT | O_WRONLY, 0666, &attr);
        if (mq == (mqd_t)-1) {
            std::perror("[Producer] mq_open initialization failed");
            return 1;
        }

        std::vector<char> wire_packet(sizeof(MessageHeader) + sz, 'X');
        auto* hdr = reinterpret_cast<MessageHeader*>(wire_packet.data());
        hdr->payload_size = static_cast<uint32_t>(sz);

        size_t current_total_target = get_total_bytes(sz);

        for (int run = 0; run <= NUM_RUNS; ++run) {
            size_t produced = 0;
            while (produced < current_total_target) {
                hdr->send_ns = now_ns();
                if (mq_send(mq, wire_packet.data(), wire_packet.size(), 0) == -1) {
                    std::perror("[Producer] mq_send operational failure");
                    break;
                }
                produced += sz;
            }
            usleep(500); 
        }
        mq_close(mq);
        mq_unlink(dynamic_name.c_str());
    }
    std::cout << "[Producer] Run complete.\n";
    return 0;
}
