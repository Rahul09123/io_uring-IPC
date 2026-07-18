#!/bin/bash
set -e

mkdir -p data

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

echo "[✓] Socket benchmarks complete. Results copied to data/"
