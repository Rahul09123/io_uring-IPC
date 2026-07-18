#!/bin/bash
set -e

MODE=${1:-"throughput"}

# 1. Compile targets cleanly
g++ -O3 -std=c++17 -Wall -o socket_producer socket_producer.cpp
g++ -O3 -std=c++17 -Wall -o socket_consumer socket_consumer.cpp

# 2. Flush out stale descriptors
rm -f /tmp/ipc_socket_bench_*

echo "======================================================"
echo " Launching Decoupled Unix Domain Socket Benchmark ($MODE) "
echo "======================================================"

# 3. Fire background consumer engine
./socket_consumer --mode "$MODE" &
CONSUMER_PID=$!

# Let the server wake up and create its listening context
sleep 1.5

# 4. Fire foreground producer client
./socket_producer --mode "$MODE"

# 5. Hold session until background worker returns
wait $CONSUMER_PID

# 6. Housekeeping cleanup
rm -f socket_producer socket_consumer
echo "[✓] Socket Evaluation ($MODE) Completed successfully."
