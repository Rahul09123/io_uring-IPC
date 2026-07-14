#!/bin/bash
set -e

# 1. Compile targets cleanly
g++ -O3 -std=c++17 -Wall -o uring_producer uring_producer.cpp -luring -lrt
g++ -O3 -std=c++17 -Wall -o uring_consumer uring_consumer.cpp -luring -lrt

# 2. Force clear out any corrupted memory segments before launching
sudo rm -f /dev/shm/ipc_uring_ring_buffer*

echo "======================================================"
echo " Launching Decoupled io_uring Shared Ring Pipeline    "
echo "======================================================"

# 3. Fire background consumer engine WITH SUDO ELEVATION
sudo ./uring_consumer &
CONSUMER_PID=$!

sleep 1.5

# 4. Fire foreground producer client WITH SUDO ELEVATION
sudo ./uring_producer

# 5. Hold session until background worker returns
wait $CONSUMER_PID

# 6. Clean up binaries
rm -f uring_producer uring_consumer
echo "[✓] io_uring Evaluation Completed successfully."
