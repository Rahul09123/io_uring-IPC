#!/bin/bash
set -e

mkdir -p data

# Ensure limits are configured if possible
ulimit -n 65535 || true
ulimit -q 209715200 || true

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

echo "[✓] Message Queue benchmarks complete. Results copied to data/"
