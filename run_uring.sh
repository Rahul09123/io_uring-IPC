#!/bin/bash
set -e

mkdir -p data

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

echo "[✓] SPSC Ring wakeup ablation benchmarks complete. Results copied to data/"
