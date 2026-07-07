#include <fcntl.h>
#include <sys/stat.h>
#include <mqueue.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sched.h>
#include <iostream>
#include <cstring>
#include <chrono>
#include <vector>
#include <fstream>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <memory>
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

struct Stats { double avg, stddev, p50, p95, p99, throughput_gbps; };

static Stats compute_stats(Telemetry* tel, size_t msg_sz) {
    Stats s{};
    if (tel->latency_count == 0) return s;
    std::vector<double> lats(tel->latencies, tel->latencies + tel->latency_count);
    lats.erase(std::remove_if(lats.begin(), lats.end(), [](double v){ return v < 0.0; }), lats.end());
    if (lats.empty()) return s;
    
    std::sort(lats.begin(), lats.end());
    double sum = std::accumulate(lats.begin(), lats.end(), 0.0);
    s.avg = sum / lats.size();
    
    double var = 0.0;
    for (double v : lats) var += (v - s.avg) * (v - s.avg);
    s.stddev = std::sqrt(var / lats.size());
    
    s.p50 = lats[lats.size() * 50 / 100];
    s.p95 = lats[lats.size() * 95 / 100];
    s.p99 = lats[lats.size() * 99 / 100];
    s.throughput_gbps = (get_total_bytes(msg_sz) / (1024.0 * 1024.0 * 1024.0)) / tel->execution_time_sec;
    return s;
}

int main() {
    set_affinity(CONSUMER_CORE);

    shm_unlink(SHM_TEL_NAME);
    int shm_fd = shm_open(SHM_TEL_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) { return 1; }
    if (ftruncate(shm_fd, sizeof(Telemetry)) < 0) { return 1; }

    auto* tel = static_cast<Telemetry*>(mmap(nullptr, sizeof(Telemetry), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0));
    if (tel == MAP_FAILED) { return 1; }

    std::ofstream csv("mq_results.csv");
    csv << "message_size_bytes,run,throughput_gbps,avg_latency_us,stddev_us,p50_us,p95_us,p99_us\n";

    std::vector<char> rx_buf(MAX_PAYLOAD + sizeof(MessageHeader) + 4096);
    std::cout << "[Consumer] Sync established. Listening for active producer loops...\n";

    for (size_t sz : MESSAGE_SIZES) {
        std::cout << "\n=========================================\n";
        std::cout << "Testing Message Size: " << sz << " Bytes\n";
        std::cout << "=========================================\n";

        std::string unique_mq_name = "/ipc_mq_bench_" + std::to_string(sz);
        size_t current_total_target = get_total_bytes(sz);

        mq_unlink(unique_mq_name.c_str());
        struct mq_attr attr{};
        attr.mq_flags   = 0;
        attr.mq_maxmsg  = 10; 
        attr.mq_msgsize = static_cast<long>(sizeof(MessageHeader) + sz);
        attr.mq_curmsgs = 0;

        mqd_t mq = mq_open(unique_mq_name.c_str(), O_CREAT | O_RDONLY, 0666, &attr);
        if (mq == (mqd_t)-1) { std::perror("mq_open failed"); return 1; }

        for (int run = 0; run <= NUM_RUNS; ++run) {
            std::memset(tel, 0, sizeof(Telemetry));
            size_t consumed = 0;
            uint64_t lat_idx = 0;

            auto wall_start = std::chrono::high_resolution_clock::now();
            while (consumed < current_total_target) {
                ssize_t bytes_read = mq_receive(mq, rx_buf.data(), rx_buf.size(), nullptr);
                if (bytes_read < 0) {
                    std::perror("mq_receive error");
                    break;
                }

                uint64_t recv_ns = now_ns();
                auto* hdr = reinterpret_cast<MessageHeader*>(rx_buf.data());

                volatile char checksum = 0;
                char* payload_data = rx_buf.data() + sizeof(MessageHeader);
                for (size_t i = 0; i < hdr->payload_size; i += 64) checksum += payload_data[i];
                (void)checksum;

                if (lat_idx < MAX_LAT_SAMPLES) {
                    tel->latencies[lat_idx++] = static_cast<double>(recv_ns - hdr->send_ns) / 1000.0;
                }
                consumed += hdr->payload_size;
            }
            auto wall_end = std::chrono::high_resolution_clock::now();
            tel->execution_time_sec = std::chrono::duration<double>(wall_end - wall_start).count();
            tel->latency_count = lat_idx;

            Stats s = compute_stats(tel, sz);
            if (run == 0) {
                std::cout << "  [Warmup]  throughput=" << s.throughput_gbps << " GB/s\n";
            } else {
                std::cout << "  Run " << run << " -> Throughput: " << s.throughput_gbps << " GB/s | p50 Latency: " << s.p50 << " us\n";
                csv << sz << "," << run << "," << s.throughput_gbps << "," << s.avg << "," 
                    << s.stddev << "," << s.p50 << "," << s.p95 << "," << s.p99 << "\n";
            }
        }
        mq_close(mq);
        mq_unlink(unique_mq_name.c_str());
    }
    csv.close();
    munmap(tel, sizeof(Telemetry));
    close(shm_fd);
    shm_unlink(SHM_TEL_NAME);
    return 0;
}
