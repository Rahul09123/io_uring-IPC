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
#include <atomic>
#include "common.h"

static void set_affinity(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
}

static inline uint64_t now_ns() {
    struct timespec ts;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
}

struct Stats { double avg, stddev, p50, p95, p99, throughput_gbps; };

static Stats compute_throughput_stats(Telemetry* tel, size_t msg_sz) {
    Stats s{};
    s.throughput_gbps = (get_total_bytes(msg_sz) / (1024.0 * 1024.0 * 1024.0)) / tel->execution_time_sec;
    return s;
}

int main(int argc, char* argv[]) {
    set_affinity(CONSUMER_CORE);

    std::string mode = "throughput";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = argv[i + 1];
        }
    }

    std::cout << "[Consumer] Initializing POSIX Message Queue Consumer (Mode: " << mode << ")...\n";

    if (mode == "latency") {
        std::vector<char> rx_buf(MAX_PAYLOAD + sizeof(MessageHeader) + 4096);

        for (size_t sz : MESSAGE_SIZES) {
            std::string mq_name_a_to_b = "/ipc_mq_a_to_b_" + std::to_string(sz);
            std::string mq_name_b_to_a = "/ipc_mq_b_to_a_" + std::to_string(sz);

            mq_unlink(mq_name_a_to_b.c_str());
            mq_unlink(mq_name_b_to_a.c_str());

            struct mq_attr attr{};
            attr.mq_flags   = 0;
            attr.mq_maxmsg  = 10; 
            attr.mq_msgsize = static_cast<long>(sizeof(MessageHeader) + sz);
            attr.mq_curmsgs = 0;

            mqd_t read_mq = mq_open(mq_name_a_to_b.c_str(), O_CREAT | O_RDONLY, 0666, &attr);
            if (read_mq == (mqd_t)-1) { std::perror("mq_open read_mq failed"); return 1; }

            mqd_t write_mq = mq_open(mq_name_b_to_a.c_str(), O_CREAT | O_WRONLY, 0666, &attr);
            if (write_mq == (mqd_t)-1) { std::perror("mq_open write_mq failed"); return 1; }

            for (int run = 0; run <= NUM_RUNS; ++run) {
                size_t num_trips = (run == 0) ? 1000 : 100000;
                for (size_t trip = 0; trip < num_trips; ++trip) {
                    ssize_t bytes_read = mq_receive(read_mq, rx_buf.data(), rx_buf.size(), nullptr);
                    if (bytes_read < 0) {
                        std::perror("mq_receive failed");
                        break;
                    }

                    // Touch payload for checksum
                    volatile char checksum = 0;
                    char* payload_data = rx_buf.data() + sizeof(MessageHeader);
                    for (size_t i = 0; i < sz; i += 64) checksum += payload_data[i];
                    (void)checksum;

                    if (mq_send(write_mq, rx_buf.data(), bytes_read, 0) == -1) {
                        std::perror("mq_send failed");
                        break;
                    }
                }
            }
            mq_close(read_mq);
            mq_close(write_mq);
            mq_unlink(mq_name_a_to_b.c_str());
            mq_unlink(mq_name_b_to_a.c_str());
        }
    } else {
        // Throughput Mode
        shm_unlink(SHM_TEL_NAME);
        int shm_fd = shm_open(SHM_TEL_NAME, O_CREAT | O_RDWR, 0666);
        if (shm_fd < 0) { return 1; }
        if (ftruncate(shm_fd, sizeof(Telemetry)) < 0) { return 1; }

        auto* tel = static_cast<Telemetry*>(mmap(nullptr, sizeof(Telemetry), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0));
        if (tel == MAP_FAILED) { return 1; }

        std::ofstream csv("mq_throughput.csv");
        csv << "message_size_bytes,run,throughput_gbps\n";

        std::vector<char> rx_buf(MAX_PAYLOAD + sizeof(MessageHeader) + 4096);
        std::cout << "[Consumer] Message Queue listener engine active...\n";

        for (size_t sz : MESSAGE_SIZES) {
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

                auto wall_start = std::chrono::high_resolution_clock::now();
                while (consumed < current_total_target) {
                    ssize_t bytes_read = mq_receive(mq, rx_buf.data(), rx_buf.size(), nullptr);
                    if (bytes_read < 0) {
                        std::perror("mq_receive error");
                        break;
                    }

                    auto* hdr = reinterpret_cast<MessageHeader*>(rx_buf.data());

                    volatile char checksum = 0;
                    char* payload_data = rx_buf.data() + sizeof(MessageHeader);
                    for (size_t i = 0; i < hdr->payload_size; i += 64) checksum += payload_data[i];
                    (void)checksum;

                    consumed += hdr->payload_size;
                }
                auto wall_end = std::chrono::high_resolution_clock::now();
                tel->execution_time_sec = std::chrono::duration<double>(wall_end - wall_start).count();

                Stats s = compute_throughput_stats(tel, sz);
                if (run == 0) {
                    std::cout << "  [Warmup]  throughput=" << s.throughput_gbps << " GB/s\n";
                } else {
                    std::cout << "  Run " << run << " -> Throughput: " << s.throughput_gbps << " GB/s\n";
                    csv << sz << "," << run << "," << s.throughput_gbps << "\n";
                }
            }
            mq_close(mq);
            mq_unlink(unique_mq_name.c_str());
        }
        csv.close();
        munmap(tel, sizeof(Telemetry));
        close(shm_fd);
        shm_unlink(SHM_TEL_NAME);
    }
    return 0;
}
