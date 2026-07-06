#!/bin/bash
set -e

# 1. Compile targets cleanly with optimized performance profiles
g++ -O3 -std=c++17 -Wall -o socket_producer socket_producer.cpp
g++ -O3 -std=c++17 -Wall -o socket_consumer socket_consumer.cpp

# 2. Flush out stale descriptors
rm -f /tmp/ipc_socket_bench_*

echo "======================================================"
echo " Launching Decoupled Unix Domain Socket Benchmark     "
echo "======================================================"

# 3. Fire background consumer engine
./socket_consumer &
CONSUMER_PID=$!

# Let the server wake up and create its listening context
sleep 1.5

# 4. Fire foreground producer client
./socket_producer

# 5. Hold session until background worker returns
wait $CONSUMER_PID

# 6. Housekeeping cleanup
rm -f socket_producer socket_consumer
echo "[✓] Socket Evaluation Completed successfully. Output written to socket_results.csv"
