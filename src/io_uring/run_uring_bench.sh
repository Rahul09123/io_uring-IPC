#!/bin/bash
set -e

MODE="throughput"
WAKEUP="uring"
REGIME="saturated"
RATE="50000"

while [[ "$#" -gt 0 ]]; do
    case $1 in
        --mode) MODE="$2"; shift ;;
        --wakeup) WAKEUP="$2"; shift ;;
        --regime) REGIME="$2"; shift ;;
        --rate) RATE="$2"; shift ;;
    esac
    shift
done

# 1. Compile targets cleanly
g++ -O3 -std=c++17 -Wall -o uring_producer uring_producer.cpp -luring -lrt
g++ -O3 -std=c++17 -Wall -o uring_consumer uring_consumer.cpp -luring -lrt

# 2. Force clear out any corrupted memory segments before launching
sudo rm -f /dev/shm/ipc_uring_ring_buffer*
sudo rm -f /tmp/uring_sig_fifo_*
sudo rm -f /tmp/uring_evfd_socket

echo "======================================================"
echo " Launching io_uring Shared Memory SPSC Ring           "
echo " Mode: $MODE | Wakeup: $WAKEUP | Regime: $REGIME "
echo "======================================================"

# 3. Fire background consumer engine
sudo ./uring_consumer --mode "$MODE" --wakeup "$WAKEUP" --regime "$REGIME" --rate "$RATE" &
CONSUMER_PID=$!

sleep 1.5

# 4. Fire foreground producer client
sudo ./uring_producer --mode "$MODE" --wakeup "$WAKEUP" --regime "$REGIME" --rate "$RATE"

# 5. Hold session until background worker returns
wait $CONSUMER_PID

# 6. Clean up binaries
rm -f uring_producer uring_consumer
echo "[✓] Shared Memory Ring SPSC Evaluation Completed successfully."
