#!/usr/bin/env bash
# run_pingpong.sh — Compile and run the corrected depth-1 ping-pong benchmark
#
# Usage:
#   bash run_pingpong.sh                        # full sweep
#   bash run_pingpong.sh --dry-run              # compile only
#   bash run_pingpong.sh --ipc pipe             # single IPC type
#   bash run_pingpong.sh --ipc shm_ablation \
#                        --variant futex \
#                        --variant io_uring     # ablation subset
#
# Prerequisites (Linux, kernel >= 5.1):
#   sudo apt install -y liburing-dev g++ libmqueue-dev python3-matplotlib
#   sudo cpupower frequency-set -g performance   # strongly recommended
#
# Output:
#   data/pingpong_<ipc>_summary.csv   — per-size stats for each IPC type
#   data/pingpong_results.csv         — merged summary (all types)
#   figures/pingpong/fig_A_*.png      — latency vs. message size
#   figures/pingpong/fig_B_*.png      — tail latency comparison
#   figures/pingpong/fig_C_*.png      — ablation wakeup variant latency

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
DATA_DIR="$REPO_ROOT/data"
FIGURES_DIR="$REPO_ROOT/figures/pingpong"

ALL_IPCS=(pipe unix_socket shm_io_uring posix_mq shm_ablation)
ALL_VARIANTS=(0 1 2 3 5)  # variant integers for pp_ablation (excluding eventfd)
VARIANT_NAMES=(busy_poll spin_backoff adaptive futex io_uring)

SELECTED_IPCS=()
SELECTED_VARIANTS=()
DRY_RUN=0

# ── Argument parsing ──────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run)  DRY_RUN=1;               shift;;
        --ipc)      SELECTED_IPCS+=("$2");   shift 2;;
        --variant)  SELECTED_VARIANTS+=("$2"); shift 2;;
        *) echo "Unknown option: $1"; exit 1;;
    esac
done

if [[ ${#SELECTED_IPCS[@]} -eq 0 ]];     then SELECTED_IPCS=("${ALL_IPCS[@]}"); fi
if [[ ${#SELECTED_VARIANTS[@]} -eq 0 ]]; then SELECTED_VARIANTS=("${VARIANT_NAMES[@]}"); fi

# ── Step 0: Warn if CPU governor is not 'performance' ────────────────────────
echo "================================================"
echo "  Step 0: Environment checks"
echo "================================================"
if command -v cpupower &>/dev/null; then
    GOV=$(cpupower frequency-info -p 2>/dev/null | grep -oP '"\K[^"]+(?=")' | tail -1 || true)
    if [[ "$GOV" != "performance" ]]; then
        echo "  WARN: CPU governor is '${GOV:-unknown}' (not 'performance')"
        echo "        Tail latency (P99/P99.9) will be inflated by frequency scaling."
        echo "        Fix: sudo cpupower frequency-set -g performance"
    else
        echo "  ✓ CPU governor: performance"
    fi
else
    echo "  WARN: cpupower not found — cannot verify CPU governor"
fi

# ── Step 1: Compile ───────────────────────────────────────────────────────────
echo ""
echo "================================================"
echo "  Step 1: Compiling ping-pong binaries"
echo "================================================"
mkdir -p "$BUILD_DIR" "$DATA_DIR" "$FIGURES_DIR"

CXX=${CXX:-g++}
CXXFLAGS="-O3 -std=c++20 -march=native -pthread -Wall -Wextra"
INCLUDES="-I$SCRIPT_DIR"
LDFLAGS="-luring -lrt"

compile_binary() {
    local src="$1" out="$2" extra_flags="${3:-}"
    $CXX $CXXFLAGS $INCLUDES "$src" -o "$out" $LDFLAGS $extra_flags
    echo "  ✓ $(basename $src) -> $out"
}

for ipc in "${SELECTED_IPCS[@]}"; do
    case "$ipc" in
        pipe)         compile_binary "$SCRIPT_DIR/pp_pipe.cpp"      "$BUILD_DIR/pp_pipe";;
        unix_socket)  compile_binary "$SCRIPT_DIR/pp_socket.cpp"    "$BUILD_DIR/pp_socket";;
        shm_io_uring) compile_binary "$SCRIPT_DIR/pp_shm_uring.cpp" "$BUILD_DIR/pp_shm_uring";;
        posix_mq)     compile_binary "$SCRIPT_DIR/pp_mq.cpp"        "$BUILD_DIR/pp_mq";;
        shm_ablation) compile_binary "$SCRIPT_DIR/pp_ablation.cpp"  "$BUILD_DIR/pp_ablation";;
    esac
done

if [[ $DRY_RUN -eq 1 ]]; then
    echo ""
    echo "  --dry-run: compile successful. Skipping execution."
    exit 0
fi

# ── Step 2: Run IPC benchmarks ────────────────────────────────────────────────
echo ""
echo "================================================"
echo "  Step 2: Running IPC ping-pong benchmarks"
echo "================================================"
cd "$DATA_DIR"

for ipc in "${SELECTED_IPCS[@]}"; do
    case "$ipc" in
        pipe)
            echo ""; echo "── Pipe ──────────────────────────────────────────"
            "$BUILD_DIR/pp_pipe"
            ;;
        unix_socket)
            echo ""; echo "── Unix Socket ──────────────────────────────────"
            "$BUILD_DIR/pp_socket"
            ;;
        shm_io_uring)
            echo ""; echo "── SHM + io_uring ───────────────────────────────"
            "$BUILD_DIR/pp_shm_uring"
            ;;
        posix_mq)
            echo ""; echo "── POSIX MQ ─────────────────────────────────────"
            # May need elevated msgsize_max
            MSGSIZE_MAX=$(cat /proc/sys/fs/mqueue/msgsize_max 2>/dev/null || echo 0)
            if [[ "$MSGSIZE_MAX" -lt 1048576 ]]; then
                echo "  WARN: /proc/sys/fs/mqueue/msgsize_max=$MSGSIZE_MAX < 1MB"
                echo "        Large-message MQ runs may fail."
                echo "        Fix: sudo sysctl -w fs.mqueue.msgsize_max=1048576"
            fi
            "$BUILD_DIR/pp_mq"
            ;;
        shm_ablation)
            echo ""; echo "── SHM Ablation (all 6 wakeup variants) ─────────"
            for vname in "${SELECTED_VARIANTS[@]}"; do
                # Resolve name → int
                vint=-1
                for i in "${!VARIANT_NAMES[@]}"; do
                    if [[ "${VARIANT_NAMES[$i]}" == "$vname" ]]; then
                        vint=$i; break
                    fi
                done
                # If user passed an int directly
                if [[ "$vname" =~ ^[0-5]$ ]]; then vint=$vname; fi
                if [[ $vint -lt 0 ]]; then
                    echo "  WARN: unknown variant '$vname', skipping"; continue
                fi
                echo ""; echo "  variant: ${VARIANT_NAMES[$vint]}"
                "$BUILD_DIR/pp_ablation" "$vint"
            done
            ;;
    esac
done

cd "$REPO_ROOT"

# ── Step 3: Merge summaries ───────────────────────────────────────────────────
echo ""
echo "================================================"
echo "  Step 3: Merging summary CSVs"
echo "================================================"
MERGED="$DATA_DIR/pingpong_results.csv"
WROTE_HEADER=0

for f in "$DATA_DIR"/pingpong_*_summary.csv; do
    [[ -f "$f" ]] || continue
    if [[ $WROTE_HEADER -eq 0 ]]; then
        head -1 "$f" > "$MERGED"
        WROTE_HEADER=1
    fi
    tail -n +2 "$f" >> "$MERGED"
done
echo "  ✓ merged -> $MERGED"

# ── Step 4: Sanity checks ─────────────────────────────────────────────────────
echo ""
echo "================================================"
echo "  Step 4: Sanity checks"
echo "================================================"
python3 - <<'PYEOF'
import csv, os, sys

DATA_DIR = os.environ.get('DATA_DIR', 'data')
merged = os.path.join(DATA_DIR, 'pingpong_results.csv')

if not os.path.isfile(merged):
    print("  SKIP: no merged CSV found")
    sys.exit(0)

issues = 0
with open(merged) as f:
    reader = csv.DictReader(f)
    for row in reader:
        try:
            ipc   = row.get('ipc_type', '')
            sz    = int(row.get('message_size_bytes', 0))
            med   = float(row.get('median_us', 0))
            p99   = float(row.get('p99_us', 0))

            # Pipe 64B should be < 50µs unloaded
            if ipc == 'pipe' and sz == 64 and med > 50:
                print(f"  WARN [{ipc} {sz}B]: median={med:.1f}µs > 50µs "
                      f"(expected < 50µs for unloaded pipe)")
                issues += 1

            # SHM io_uring 256KB: RTT/2 must be >= transfer time
            # At 20.6 GiB/s, 256KB takes ~12µs one-way; RTT/2 must be >= 12µs
            if ipc in ('shm_io_uring', 'shm_ablation') and sz == 262144:
                half_rtt = med / 2.0
                if half_rtt < 10.0:
                    print(f"  WARN [{ipc} {sz}B]: RTT/2={half_rtt:.2f}µs < 10µs "
                          f"(transfer time alone should be ~12µs at 20 GiB/s)")
                    issues += 1

            # P99 should always >= median
            if p99 < med:
                print(f"  WARN [{ipc} {sz}B]: p99={p99:.2f} < median={med:.2f}")
                issues += 1
        except (ValueError, KeyError):
            continue

if issues == 0:
    print("  ✓ All sanity checks passed")
else:
    print(f"  {issues} sanity check(s) FAILED — review data before publishing")
PYEOF

# ── Step 5: Generate figures ──────────────────────────────────────────────────
echo ""
echo "================================================"
echo "  Step 5: Generating figures"
echo "================================================"
if [[ -f "$REPO_ROOT/scripts/pingpong_analysis.py" ]]; then
    DATA_DIR="$DATA_DIR" python3 "$REPO_ROOT/scripts/pingpong_analysis.py" \
        --data "$MERGED" \
        --output "$FIGURES_DIR"
    echo "  ✓ figures -> $FIGURES_DIR"
else
    echo "  WARN: scripts/pingpong_analysis.py not found"
fi

echo ""
echo "================================================"
echo "  Ping-pong benchmark complete!"
echo "  Results : $MERGED"
echo "  Figures : $FIGURES_DIR"
echo "================================================"
echo ""
echo "Key files to send back:"
echo "  $MERGED"
for f in "$FIGURES_DIR"/fig_*.png; do echo "  $f"; done
