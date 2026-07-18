#!/bin/bash
set -e

mkdir -p data

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

echo "[✓] Pipe benchmarks complete. Results copied to data/"
