#ifndef PINGPONG_CLOCK_UTIL_H
#define PINGPONG_CLOCK_UTIL_H

// ── High-resolution, fenced clock ────────────────────────────────────────────
//
// Requirements (from the reviewer):
//  1. CLOCK_MONOTONIC_RAW — no NTP/adjtime adjustments, monotonic.
//  2. Compiler fence asm("" ::: "memory") before AND after the call to prevent
//     the compiler reordering load/store instructions across the timestamp.
//  3. Both t_start and t_end are taken on the SAME core (Core A / initiator).
//     We never subtract timestamps from different cores — per-core TSCs are not
//     guaranteed to be synchronized.
//
// NOTE: This is NOT rdtscp. CLOCK_MONOTONIC_RAW via vDSO is ~20–40 ns overhead
// on modern Linux (no syscall, no kernel entry), which is acceptable for RTT
// measurements in the microsecond range. Use rdtscp only if you calibrate the
// TSC frequency yourself and verify invariant TSC is set in /proc/cpuinfo.

#include <cstdint>
#include <time.h>

static inline uint64_t pp_now_ns() {
    // Compiler barrier: prevents the compiler from reordering memory
    // operations around this timestamp.
    __asm__ volatile("" ::: "memory");

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);

    __asm__ volatile("" ::: "memory");

    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
           + static_cast<uint64_t>(ts.tv_nsec);
}

// Convert nanoseconds to microseconds
static inline double ns_to_us(uint64_t ns) {
    return static_cast<double>(ns) / 1000.0;
}

// CPU affinity helper
#include <sched.h>
static inline void pp_set_affinity(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
}

// IO helpers — partial-read/write safe
#include <unistd.h>
static inline bool pp_write_all(int fd, const void* buf, size_t n) {
    const char* p = static_cast<const char*>(buf);
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return false;
        p += w; n -= static_cast<size_t>(w);
    }
    return true;
}

static inline bool pp_read_all(int fd, void* buf, size_t n) {
    char* p = static_cast<char*>(buf);
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) return false;
        p += r; n -= static_cast<size_t>(r);
    }
    return true;
}

// Pause hint for spin loops
#if defined(__x86_64__) || defined(__i386__)
#  include <immintrin.h>
#  define PP_PAUSE() _mm_pause()
#else
#  define PP_PAUSE() ((void)0)
#endif

#endif // PINGPONG_CLOCK_UTIL_H
