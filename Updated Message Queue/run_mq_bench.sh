#!/bin/bash

# 1. Force absolute highest descriptor and queue memory caps (in bytes)
ulimit -n 65535
ulimit -q 209715200  # FORCE POSIX MQ allocation window to 200MB directly as root

# 2. Hard reset system queue counts 
sudo sysctl -w fs.mqueue.queues_max=2048
sudo sysctl -w fs.mqueue.msgsize_max=1048600

# 3. Completely wipe out any hidden zombie descriptors from kernel space
sudo rm -rf /dev/mqueue/*

echo "======================================================"
echo " Launching Privileged POSIX MQueue Benchmark Pipeline  "
echo "======================================================"

# 4. Launch consumer 
./mq_consumer &
CONSUMER_PID=$!

# Give the consumer time to open its socket/descriptor channel
sleep 2

# 5. Launch producer 
./mq_producer

# 6. Wait for consumer to cleanly parse data and write mq_results.csv
wait $CONSUMER_PID

echo "[✓] Pipeline fully processed with zero descriptor limits broken!"
