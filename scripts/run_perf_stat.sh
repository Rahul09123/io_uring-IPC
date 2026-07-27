#!/bin/bash
set -e

mkdir -p data

# Ensure we are in the correct directory
cd "$(dirname "$0")/../src/ablation/build"

# Run each variant's consumer under perf stat
for variant in busy_poll spin_backoff adaptive futex eventfd io_uring; do
    echo "Running perf stat for consumer ($variant, 64B, bursty)"
    
    # variant_int mapping: 0=busy_poll, 1=spin_backoff, 2=adaptive, 3=futex, 4=eventfd, 5=io_uring
    V_INT=0
    case "$variant" in
        "busy_poll") V_INT=0 ;;
        "spin_backoff") V_INT=1 ;;
        "adaptive") V_INT=2 ;;
        "futex") V_INT=3 ;;
        "eventfd") V_INT=4 ;;
        "io_uring") V_INT=5 ;;
    esac

    # Clean up stale IPC objects (same as run_ablation.sh)
    rm -f /tmp/ablation_sig_fifo /tmp/ablation_consumer_pid /dev/shm/ipc_ablation_ring 2>/dev/null || true
    
    # Start consumer under perf stat (argv[1]=variant_int, argv[2]=csv_out)
    perf stat -e cycles,instructions,context-switches,cpu-migrations,task-clock \
        -o ../../../data/perf_${variant}_bursty_64B.txt \
        ./ablation_consumer $V_INT /dev/null &
    
    CONSUMER_PID=$!
    
    # Wait a moment for consumer to be ready
    sleep 1.5
    

    
    echo "  Starting producer..."
    ./ablation_producer $V_INT bursty
    
    # Wait for consumer to finish processing and exit
    wait $CONSUMER_PID
done

echo "Done running perf stat."
