#!/bin/bash
set -e

MODE=${1:-"throughput"}

# 1. Compile targets cleanly
g++ -O3 -std=c++17 -Wall -o mq_producer mq_producer.cpp -lrt
g++ -O3 -std=c++17 -Wall -o mq_consumer mq_consumer.cpp -lrt

# 2. Force absolute highest descriptor and queue memory caps (in bytes)
ulimit -n 65535
ulimit -q 209715200

# 3. Hard reset system queue counts
sudo sysctl -w fs.mqueue.queues_max=2048
sudo sysctl -w fs.mqueue.msgsize_max=1048600

# 4. Wipe out any hidden zombie descriptors from kernel space
sudo rm -rf /dev/mqueue/*

echo "======================================================"
echo " Launching Privileged POSIX MQueue Benchmark Pipeline ($MODE)  "
echo "======================================================"

# 4. Launch consumer 
./mq_consumer --mode "$MODE" &
CONSUMER_PID=$!

# Give the consumer time to open its socket/descriptor channel
sleep 2

# 5. Launch producer 
./mq_producer --mode "$MODE"

# 6. Wait for consumer to cleanly parse data
wait $CONSUMER_PID

# 7. Cleanup binaries
rm -f mq_producer mq_consumer

echo "[✓] Message Queue Pipeline ($MODE) completed successfully."
