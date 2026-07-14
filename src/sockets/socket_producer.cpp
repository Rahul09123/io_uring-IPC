#include <sys/socket.h>
#include <sys/un.h>
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

int main() {
    set_affinity(PRODUCER_CORE);
    std::cout << "[Producer] Initializing Unix Domain Socket Benchmark Loops...\n";

    for (size_t sz : MESSAGE_SIZES) {
        std::cout << "[Producer] Processing Size Target: " << sz << " Bytes\n";
        std::string dynamic_path = std::string(SOCKET_PATH) + "_" + std::to_string(sz);

        std::vector<char> wire(sizeof(MessageHeader) + sz);
        auto* hdr = reinterpret_cast<MessageHeader*>(wire.data());
        char* dat = wire.data() + sizeof(MessageHeader);
        std::memset(dat, 'X', sz);
        hdr->payload_size = static_cast<uint32_t>(sz);

        for (int run = 0; run <= NUM_RUNS; ++run) {
            int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (sockfd < 0) {
                std::perror("[Producer] socket creation failed");
                return 1;
            }

            int buf_sz = static_cast<int>(MAX_PAYLOAD * 2);
            setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &buf_sz, sizeof(buf_sz));

            struct sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, dynamic_path.c_str(), sizeof(addr.sun_path) - 1);

            // Active sync retry loop until consumer sets up server socket listener
            while (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                usleep(500);
            }

            size_t produced = 0;
            while (produced < TOTAL_BYTES) {
                hdr->send_ns = now_ns();
                if (!write_all(sockfd, wire.data(), wire.size())) {
                    std::cerr << "[Producer] send failure\n";
                    break;
                }
                produced += sz;
            }
            close(sockfd);
            usleep(1000); // Cooldown to let the consumer settle metrics
        }
    }
    std::cout << "[Producer] Socket Run Complete.\n";
    return 0;
}
