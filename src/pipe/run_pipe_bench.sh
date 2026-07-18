#!/bin/bash
set -e

MODE=${1:-"throughput"}

# 1. Compile targets cleanly
g++ -O3 -std=c++17 -Wall -o pipe_producer pipe_producer.cpp -lrt
g++ -O3 -std=c++17 -Wall -o pipe_consumer pipe_consumer.cpp -lrt

# 2. Clear out any jammed pipe nodes or old segment allocations
sudo rm -f /tmp/ipc_pipe_bench_*
sudo rm -f /dev/shm/ipc_pipe_telemetry*

echo "======================================================"
echo " Launching Decoupled POSIX Pipe (FIFO) Benchmark ($MODE) "
echo "======================================================"

# 3. Launch background consumer
sudo ./pipe_consumer --mode "$MODE" &
CONSUMER_PID=$!

# Let the consumer safely map the memory and create the FIFO endpoints
sleep 1.5

# 4. Fire foreground producer client
sudo ./pipe_producer --mode "$MODE"

# 5. Hold session until background worker returns
wait $CONSUMER_PID

# 6. Housekeeping cleanup
rm -f pipe_producer pipe_consumer
echo "[✓] Pipe Evaluation ($MODE) Completed successfully."
