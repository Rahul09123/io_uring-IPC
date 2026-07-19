#!/usr/bin/env bash
# run_ablation.sh — Compile, run, and analyse the wakeup-mechanism ablation.
#
# Usage:
#   bash run_ablation.sh                      # full sweep
#   bash run_ablation.sh --dry-run            # compile only
#   bash run_ablation.sh --variant futex \
#                        --variant io_uring \ # subset of variants
#                        --regime saturated \ # subset of regimes
#                        --regime bursty
#
# Requirements (Linux, kernel ≥ 5.1):
#   sudo apt install -y liburing-dev linux-perf g++ python3-matplotlib python3-numpy
#   sudo sysctl -w kernel.perf_event_paranoid=0   # for perf stat (optional)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
DATA_DIR="$REPO_ROOT/data"
FIGURES_DIR="$REPO_ROOT/figures/ablation"

ALL_VARIANTS=(busy_poll spin_backoff adaptive futex io_uring)
ALL_REGIMES=(saturated bursty offered_25 offered_50 offered_75 offered_90)

SELECTED_VARIANTS=()
SELECTED_REGIMES=()
DRY_RUN=0
USE_PERF=0

# ── Argument parsing ──────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run)     DRY_RUN=1;           shift;;
        --perf)        USE_PERF=1;          shift;;
        --variant)     SELECTED_VARIANTS+=("$2"); shift 2;;
        --regime)      SELECTED_REGIMES+=("$2");  shift 2;;
        *) echo "Unknown option: $1"; exit 1;;
    esac
done

VARIANTS=("${SELECTED_VARIANTS[@]:-${ALL_VARIANTS[@]}}")
REGIMES=("${SELECTED_REGIMES[@]:-${ALL_REGIMES[@]}}")
if [[ ${#SELECTED_VARIANTS[@]} -eq 0 ]]; then VARIANTS=("${ALL_VARIANTS[@]}"); fi
if [[ ${#SELECTED_REGIMES[@]} -eq 0 ]];  then REGIMES=("${ALL_REGIMES[@]}");  fi

# ── Variant name → integer ────────────────────────────────────────────────────
variant_to_int() {
    case "$1" in
        busy_poll)    echo 0;;
        spin_backoff) echo 1;;
        adaptive)     echo 2;;
        futex)        echo 3;;
        io_uring)     echo 4;;
        *) echo "Unknown variant: $1" >&2; exit 1;;
    esac
}

# ── Step 1: Compile ───────────────────────────────────────────────────────────
echo "================================================"
echo "  Step 1: Compiling ablation binaries"
echo "================================================"
mkdir -p "$BUILD_DIR" "$DATA_DIR" "$FIGURES_DIR"

CXX=${CXX:-g++}
CXXFLAGS="-O3 -std=c++17 -march=native -pthread -Wall -Wextra"
LDFLAGS="-luring -lrt"

$CXX $CXXFLAGS -I"$SCRIPT_DIR" \
    "$SCRIPT_DIR/consumer.cpp" \
    -o "$BUILD_DIR/ablation_consumer" $LDFLAGS

$CXX $CXXFLAGS -I"$SCRIPT_DIR" \
    "$SCRIPT_DIR/producer.cpp" \
    -o "$BUILD_DIR/ablation_producer" $LDFLAGS

echo "  ✓ consumer -> $BUILD_DIR/ablation_consumer"
echo "  ✓ producer -> $BUILD_DIR/ablation_producer"

if [[ $DRY_RUN -eq 1 ]]; then
    echo "  --dry-run: Skipping execution. Compile successful."
    exit 0
fi

# ── Step 2: Run all (variant × regime) combinations ──────────────────────────
echo ""
echo "================================================"
echo "  Step 2: Running ablation sweep"
echo "  Variants : ${VARIANTS[*]}"
echo "  Regimes  : ${REGIMES[*]}"
echo "================================================"

PERF_OUTPUT="$DATA_DIR/ablation_perf_stat.txt"
> "$PERF_OUTPUT"

for v in "${VARIANTS[@]}"; do
    V_INT=$(variant_to_int "$v")
    for r in "${REGIMES[@]}"; do
        CSV_OUT="$DATA_DIR/ablation_${v}_${r}.csv"
        echo ""
        echo "── ${v} / ${r} ────────────────────────────────────"

        # Clean up stale IPC objects
        rm -f /tmp/ablation_sig_fifo /tmp/ablation_consumer_pid /dev/shm/ipc_ablation_ring 2>/dev/null || true
        # shm_unlink via shell
        [ -e /dev/shm/ipc_ablation_ring ] && rm -f /dev/shm/ipc_ablation_ring || true

        if [[ $USE_PERF -eq 1 ]]; then
            # consumer under perf stat
            perf stat -e syscalls:sys_enter_futex,syscalls:sys_enter_read,\
syscalls:sys_enter_write,context-switches \
                -o /tmp/ablation_perf_tmp.txt -- \
                "$BUILD_DIR/ablation_consumer" "$V_INT" "$CSV_OUT" &
        else
            "$BUILD_DIR/ablation_consumer" "$V_INT" "$CSV_OUT" &
        fi
        CONSUMER_PID=$!

        sleep 1   # give consumer time to create SHM + FIFO

        "$BUILD_DIR/ablation_producer" "$V_INT" "$r"
        wait $CONSUMER_PID

        if [[ $USE_PERF -eq 1 ]] && [[ -f /tmp/ablation_perf_tmp.txt ]]; then
            echo "=== ${v} / ${r} ===" >> "$PERF_OUTPUT"
            cat /tmp/ablation_perf_tmp.txt >> "$PERF_OUTPUT"
        fi

        echo "  ✓ saved $CSV_OUT"
    done
done

# ── Step 3: Merge CSVs ────────────────────────────────────────────────────────
echo ""
echo "================================================"
echo "  Step 3: Merging CSVs"
echo "================================================"

# Patch regime column: each file is named ablation_<variant>_<regime>.csv
# The consumer writes "unknown" for regime; we fix it here.
MERGED="$DATA_DIR/ablation_results.csv"
WROTE_HEADER=0

for csv_file in "$DATA_DIR"/ablation_*.csv; do
    [[ -f "$csv_file" ]] || continue
    # Extract regime from filename: ablation_<variant>_<regime>.csv
    basename_no_ext="${csv_file%.csv}"
    basename_no_ext="${basename_no_ext##*/}"   # strip path
    # Remove leading "ablation_"
    rest="${basename_no_ext#ablation_}"
    # Variant is first token (may have underscore), regime is after last underscore-set
    # We know variant names: busy_poll spin_backoff adaptive futex eventfd io_uring
    detected_regime=""
    for r in "${ALL_REGIMES[@]}"; do
        if [[ "$rest" == *_"$r" ]]; then
            detected_regime="$r"
            break
        fi
    done
    [[ -z "$detected_regime" ]] && continue

    if [[ $WROTE_HEADER -eq 0 ]]; then
        head -1 "$csv_file" > "$MERGED"
        WROTE_HEADER=1
    fi

    # Replace "unknown" in regime column (col 2) with actual regime
    tail -n +2 "$csv_file" \
        | awk -F',' -v OFS=',' -v rg="$detected_regime" \
              '{ $2 = rg; print }' >> "$MERGED"
done

echo "  ✓ merged -> $MERGED"

# ── Step 4: Regression check (IO_URING/saturated vs existing data) ────────────
echo ""
echo "================================================"
echo "  Step 4: Regression check"
echo "================================================"
if [[ -f "$DATA_DIR/io_uring_results.csv" ]] && [[ -f "$DATA_DIR/ablation_io_uring_saturated.csv" ]]; then
    python3 - <<'PYEOF'
import csv, sys, os

def read_p50(path, size_filter=None):
    vals = []
    with open(path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                sz = int(row.get('message_size_bytes', 0))
                if size_filter and sz != size_filter:
                    continue
                # try common column names
                p50 = float(row.get('p50_us', row.get('p50', 0)))
                if p50 > 0:
                    vals.append(p50)
            except (ValueError, KeyError):
                continue
    return sum(vals)/len(vals) if vals else None

repo = os.environ.get('REPO_ROOT', os.path.join(os.path.dirname(__file__), '..', '..'))
old_csv = os.path.join(repo, 'data', 'io_uring_results.csv')
new_csv = os.path.join(repo, 'data', 'ablation_io_uring_saturated.csv')

old_p50 = read_p50(old_csv)
new_p50 = read_p50(new_csv)

if old_p50 and new_p50:
    diff_pct = abs(new_p50 - old_p50) / old_p50 * 100
    status = 'PASS' if diff_pct < 5 else 'WARN'
    print(f"  Regression [{status}]: old p50={old_p50:.2f} µs  "
          f"new p50={new_p50:.2f} µs  diff={diff_pct:.1f}%")
else:
    print("  Regression: skipped (could not read p50 from one or both files)")
PYEOF
else
    echo "  Regression: skipped (io_uring_results.csv not found)"
fi

# ── Step 5: Generate figures ──────────────────────────────────────────────────
echo ""
echo "================================================"
echo "  Step 5: Generating figures"
echo "================================================"
if [[ -f "$REPO_ROOT/scripts/ablation_analysis.py" ]]; then
    python3 "$REPO_ROOT/scripts/ablation_analysis.py" \
        --data "$MERGED" \
        --perf "$PERF_OUTPUT" \
        --output "$FIGURES_DIR"
    echo "  ✓ figures -> $FIGURES_DIR"
else
    echo "  WARN: scripts/ablation_analysis.py not found, skipping figures"
fi

echo ""
echo "================================================"
echo "  Ablation complete!"
echo "  Results: $MERGED"
echo "  Figures: $FIGURES_DIR"
echo "================================================"
