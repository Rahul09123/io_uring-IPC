#ifndef PINGPONG_STATS_UTIL_H
#define PINGPONG_STATS_UTIL_H

// ── Latency distribution calculator for ping-pong benchmarks ─────────────────
//
// Input  : vector of RTT values in nanoseconds (raw, including warmup)
// Output : PPStats struct with all percentiles in microseconds
//
// Statistics reported:
//  mean, median (p50), p90, p99, p99.9 (p999)
//  95% CI: mean ± 1.96 × σ/√n  (large-sample Gaussian approximation, valid n≥10000)
//
// Negative values are filtered (clock anomalies / non-monotonic reads).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

struct PPStats {
    double mean_us   = 0.0;
    double median_us = 0.0;
    double p90_us    = 0.0;
    double p99_us    = 0.0;
    double p999_us   = 0.0;
    double ci95_lo_us = 0.0;
    double ci95_hi_us = 0.0;
    size_t n          = 0;
};

// pct: 0.0–100.0
static inline double percentile(const std::vector<double>& sorted_v, double pct) {
    if (sorted_v.empty()) return 0.0;
    if (pct <= 0.0)   return sorted_v.front();
    if (pct >= 100.0) return sorted_v.back();
    double idx = pct / 100.0 * static_cast<double>(sorted_v.size() - 1);
    size_t lo  = static_cast<size_t>(idx);
    size_t hi  = lo + 1;
    if (hi >= sorted_v.size()) return sorted_v.back();
    double frac = idx - static_cast<double>(lo);
    return sorted_v[lo] * (1.0 - frac) + sorted_v[hi] * frac;
}

static inline PPStats compute_pp_stats(const std::vector<uint64_t>& rtt_ns) {
    PPStats s{};
    if (rtt_ns.empty()) return s;

    // Convert to µs, filter negatives / outlier zeros
    std::vector<double> us;
    us.reserve(rtt_ns.size());
    for (uint64_t v : rtt_ns) {
        double us_val = static_cast<double>(v) / 1000.0;
        if (us_val > 0.0)
            us.push_back(us_val);
    }
    if (us.empty()) return s;

    std::sort(us.begin(), us.end());
    s.n = us.size();

    // mean
    double sum = 0.0;
    for (double v : us) sum += v;
    s.mean_us = sum / static_cast<double>(s.n);

    // standard deviation
    double var = 0.0;
    for (double v : us) var += (v - s.mean_us) * (v - s.mean_us);
    double stddev = std::sqrt(var / static_cast<double>(s.n));

    // percentiles (linear interpolation)
    s.median_us = percentile(us, 50.0);
    s.p90_us    = percentile(us, 90.0);
    s.p99_us    = percentile(us, 99.0);
    s.p999_us   = percentile(us, 99.9);

    // 95% CI: mean ± 1.96 × σ/√n
    double margin = 1.96 * stddev / std::sqrt(static_cast<double>(s.n));
    s.ci95_lo_us = s.mean_us - margin;
    s.ci95_hi_us = s.mean_us + margin;

    return s;
}

// Write one summary row to a CSV stream
#include <ostream>
#include <string>
static inline void write_summary_row(std::ostream& out,
                                     const std::string& ipc_type,
                                     const std::string& wakeup_variant,
                                     size_t msg_sz,
                                     const PPStats& s) {
    out << ipc_type << ","
        << wakeup_variant << ","
        << msg_sz << ","
        << s.n << ","
        << s.mean_us   << ","
        << s.median_us << ","
        << s.p90_us    << ","
        << s.p99_us    << ","
        << s.p999_us   << ","
        << s.ci95_lo_us << ","
        << s.ci95_hi_us << "\n";
}

#endif // PINGPONG_STATS_UTIL_H
