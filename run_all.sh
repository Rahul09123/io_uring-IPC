#!/bin/bash
set -e

echo "========================================================="
echo " STARTING COMPLETE IPC ABLATION & MULTI-MODE BENCHMARK  "
echo "========================================================="

# Create data directory if it doesn't exist
mkdir -p data

# --- 1. RUN PIPE BENCHMARKS ---
echo "Running Pipe in Throughput mode..."
cd src/pipe
bash run_pipe_bench.sh throughput
cp -f pipe_throughput.csv ../../data/
cd ../..

echo "Running Pipe in Latency mode..."
cd src/pipe
bash run_pipe_bench.sh latency
cp -f pipe_latency.csv ../../data/
cd ../..


# --- 2. RUN UNIX SOCKET BENCHMARKS ---
echo "Running Unix Sockets in Throughput mode..."
cd src/sockets
bash run_socket_bench.sh throughput
cp -f socket_throughput.csv ../../data/
cd ../..

echo "Running Unix Sockets in Latency mode..."
cd src/sockets
bash run_socket_bench.sh latency
cp -f socket_latency.csv ../../data/
cd ../..


# --- 3. RUN POSIX MESSAGE QUEUE BENCHMARKS ---
echo "Running POSIX MQ in Throughput mode..."
cd src/mq
bash run_mq_bench.sh throughput
cp -f mq_throughput.csv ../../data/
cd ../..

echo "Running POSIX MQ in Latency mode..."
cd src/mq
bash run_mq_bench.sh latency
cp -f mq_latency.csv ../../data/
cd ../..


# --- 4. RUN SHM SPSC RING WAKEUP ABLATION BENCHMARKS ---
VARIANTS=("spin" "backoff" "adaptive" "futex" "eventfd" "uring")

for var in "${VARIANTS[@]}"; do
    echo "Running SPSC Ring with Wakeup: $var (Throughput)..."
    cd src/io_uring
    bash run_uring_bench.sh --mode throughput --wakeup "$var"
    cp -f "uring_${var}_throughput.csv" ../../data/
    cd ../..
done

for var in "${VARIANTS[@]}"; do
    echo "Running SPSC Ring with Wakeup: $var (Latency)..."
    cd src/io_uring
    bash run_uring_bench.sh --mode latency --wakeup "$var"
    cp -f "uring_${var}_latency.csv" ../../data/
    cd ../..
done


# --- 5. RUN ANALYSIS & VISUALIZATIONS ---
echo "Running statistical analysis and generating visualizations..."
python3 scripts/generate_visualizations.py
python3 scripts/statistical_analysis.py

echo "========================================================="
echo " [✓] ALL BENCHMARKS COMPLETED & FIGURES GENERATED      "
echo "========================================================="
