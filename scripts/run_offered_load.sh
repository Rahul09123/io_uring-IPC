#!/bin/bash
set -e

mkdir -p data

cd "$(dirname "$0")/../src/ablation/build"

# We run the offered load sweep at 64B
echo "Running offered-load sweep (25%, 50%, 75%, 90%)..."

for pct in 25 50 75 90; do
    regime="offered_${pct}"
    for variant in busy_poll spin_backoff adaptive futex eventfd io_uring; do
        echo "  - $variant at $pct%"
        
        # Start consumer in the background
        # Usage: ./ablation_consumer <size> <variant_int>
        V_INT=0
        case "$variant" in
            "busy_poll") V_INT=0 ;;
            "spin_backoff") V_INT=1 ;;
            "adaptive") V_INT=2 ;;
            "futex") V_INT=3 ;;
            "eventfd") V_INT=4 ;;
            "io_uring") V_INT=5 ;;
        esac
        
        ./ablation_consumer 64 $V_INT > /dev/null 2>&1 &
        CONSUMER_PID=$!
        
        sleep 0.5
        
        # Producer writes CSV output to stdout, we capture it
        ./ablation_producer $V_INT $regime > "../../../data/offered_${variant}_64B_${pct}pct.csv" 2>/dev/null
        
        wait $CONSUMER_PID || true
    done
done

echo "Done running offered-load sweep."
