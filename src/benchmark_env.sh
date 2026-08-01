#!/usr/bin/env bash
# Shared, non-privileged environment capture for reproducible benchmark runs.
# It records settings but never changes them; configure the host before a run.

benchmark_capture_environment() {
    local output_path="$1" require_fixed_frequency="$2"
    local cores="${BENCHMARK_CORES:-1 2}" governor="" turbo="unknown" failure=0

    if [[ "$(uname -s)" != "Linux" ]]; then
        echo "ERROR: benchmarks require Linux." >&2; return 1
    fi
    for core in $cores; do
        local governor_file="/sys/devices/system/cpu/cpu${core}/cpufreq/scaling_governor"
        if [[ ! -r "$governor_file" ]]; then
            echo "WARN: cannot read $governor_file" >&2; failure=1; continue
        fi
        local current_governor="$(<"$governor_file")"
        governor+="cpu${core}=${current_governor} "
        if [[ "$current_governor" != "performance" ]]; then
            echo "WARN: cpu${core} governor is '$current_governor', not performance." >&2; failure=1
        fi
    done
    if [[ -r /sys/devices/system/cpu/intel_pstate/no_turbo ]]; then
        turbo="intel_pstate.no_turbo=$(</sys/devices/system/cpu/intel_pstate/no_turbo)"
        [[ "$(</sys/devices/system/cpu/intel_pstate/no_turbo)" == "1" ]] || failure=1
    elif [[ -r /sys/devices/system/cpu/cpufreq/boost ]]; then
        turbo="cpufreq.boost=$(</sys/devices/system/cpu/cpufreq/boost)"
        [[ "$(</sys/devices/system/cpu/cpufreq/boost)" == "0" ]] || failure=1
    else
        echo "WARN: cannot determine turbo/boost state on this host." >&2; failure=1
    fi
    mkdir -p "$(dirname "$output_path")"
    {
        echo "timestamp_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "hostname=$(hostname)"
        echo "kernel=$(uname -srmo)"
        echo "cpu_model=$(lscpu 2>/dev/null | awk -F: '/Model name/ {sub(/^[[:space:]]+/, \"\", $2); print $2; exit}')"
        echo "benchmark_cores=$cores"
        echo "governor=$governor"
        echo "turbo=$turbo"
        echo "require_fixed_frequency=$require_fixed_frequency"
    } > "$output_path"
    echo "  Environment record: $output_path"
    if [[ "$require_fixed_frequency" == "1" && $failure -ne 0 ]]; then
        echo "ERROR: fixed-frequency checks failed; configure performance governor and disable turbo/boost." >&2
        return 1
    fi
}
