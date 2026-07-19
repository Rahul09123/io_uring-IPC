#ifndef ABLATION_WAKEUP_H
#define ABLATION_WAKEUP_H

// ── All 6 wakeup variants — selected at runtime via the WakeupVariant enum ───
//
// Each namespace exposes two inline functions with identical signatures:
//
//   void consumer_wait(RingBuffer* rb, WakeupState& ws,
//                      struct io_uring* ring, uint64_t t)
//
//   void producer_signal(RingBuffer* rb, WakeupState& ws,
//                        struct io_uring* ring, uint64_t* wakeup_count)
//
// consumer_wait is called when the ring is empty (head == tail).
// producer_signal is called after every head advance (guarded by the
// consumer_sleeping CAS, same logic as the original implementation).

#include "ring.h"
#include "common.h"

#include <atomic>
#include <cstdint>
#include <unistd.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <sched.h>
#include <liburing.h>

// arch-specific pause
#if defined(__x86_64__) || defined(__i386__)
#  include <immintrin.h>
#  define CPU_PAUSE() _mm_pause()
#else
#  define CPU_PAUSE() ((void)0)
#endif

// ── futex helpers ─────────────────────────────────────────────────────────────
static inline int futex_wait(uint32_t* addr, uint32_t val) {
    return static_cast<int>(
        syscall(SYS_futex, addr, FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
                val, nullptr, nullptr, 0));
}
static inline int futex_wake(uint32_t* addr, int n) {
    return static_cast<int>(
        syscall(SYS_futex, addr, FUTEX_WAKE | FUTEX_PRIVATE_FLAG,
                n, nullptr, nullptr, 0));
}

// ── now_ns helper ─────────────────────────────────────────────────────────────
#include <chrono>
static inline uint64_t wakeup_now_ns() {
    return static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
}

// ─────────────────────────────────────────────────────────────────────────────
// 0: BUSY_POLL
// ─────────────────────────────────────────────────────────────────────────────
namespace BusyPoll {
    static inline void consumer_wait(RingBuffer* /*rb*/, WakeupState& /*ws*/,
                                     struct io_uring* /*ring*/, uint64_t /*t*/) {
        // caller should re-check the ring immediately — nothing to do here
    }
    static inline void producer_signal(RingBuffer* /*rb*/, WakeupState& /*ws*/,
                                       struct io_uring* /*ring*/,
                                       uint64_t* /*wakeup_count*/) {
        // no signaling needed — consumer never sleeps
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 1: SPIN_BACKOFF — spin + _mm_pause + exponential backoff + sched_yield
// ─────────────────────────────────────────────────────────────────────────────
namespace SpinBackoff {
    static inline void consumer_wait(RingBuffer* /*rb*/, WakeupState& /*ws*/,
                                     struct io_uring* /*ring*/, uint64_t /*t*/) {
        // caller is in a tight loop; just emit a pause hint
        CPU_PAUSE();
    }
    static inline void producer_signal(RingBuffer* /*rb*/, WakeupState& /*ws*/,
                                       struct io_uring* /*ring*/,
                                       uint64_t* /*wakeup_count*/) {
        // no explicit signal — consumer spins
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 2: ADAPTIVE — spin N iterations, then fall back to futex
// ─────────────────────────────────────────────────────────────────────────────
namespace Adaptive {
    // thread-local spin counter (reset per empty-ring episode)
    static thread_local int spin_count = 0;

    static inline void consumer_wait(RingBuffer* rb, WakeupState& /*ws*/,
                                     struct io_uring* /*ring*/, uint64_t t) {
        if (spin_count < ADAPTIVE_SPIN_ITERS) {
            ++spin_count;
            CPU_PAUSE();
            return;
        }
        // fall through to futex
        spin_count = 0;
        rb->consumer_sleeping.store(1, std::memory_order_seq_cst);
        if (rb->head.load(std::memory_order_seq_cst) != t) {
            uint32_t expected = 1;
            rb->consumer_sleeping.compare_exchange_strong(
                expected, 0, std::memory_order_seq_cst);
            return;
        }
        uint32_t* addr = reinterpret_cast<uint32_t*>(&rb->consumer_sleeping);
        futex_wait(addr, 1);
    }

    static inline void producer_signal(RingBuffer* rb, WakeupState& /*ws*/,
                                       struct io_uring* /*ring*/,
                                       uint64_t* wakeup_count) {
        if (rb->consumer_sleeping.load(std::memory_order_seq_cst) == 1) {
            uint32_t expected = 1;
            if (rb->consumer_sleeping.compare_exchange_strong(
                    expected, 0, std::memory_order_acq_rel)) {
                ++(*wakeup_count);
                uint32_t* addr = reinterpret_cast<uint32_t*>(&rb->consumer_sleeping);
                futex_wake(addr, 1);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 3: FUTEX — direct futex(FUTEX_WAIT) / futex(FUTEX_WAKE)
// ─────────────────────────────────────────────────────────────────────────────
namespace FutexWakeup {
    static inline void consumer_wait(RingBuffer* rb, WakeupState& /*ws*/,
                                     struct io_uring* /*ring*/, uint64_t t) {
        rb->consumer_sleeping.store(1, std::memory_order_seq_cst);
        // double-check
        if (rb->head.load(std::memory_order_seq_cst) != t) {
            uint32_t expected = 1;
            rb->consumer_sleeping.compare_exchange_strong(
                expected, 0, std::memory_order_seq_cst);
            return;
        }
        uint32_t* addr = reinterpret_cast<uint32_t*>(&rb->consumer_sleeping);
        futex_wait(addr, 1);
    }

    static inline void producer_signal(RingBuffer* rb, WakeupState& /*ws*/,
                                       struct io_uring* /*ring*/,
                                       uint64_t* wakeup_count) {
        if (rb->consumer_sleeping.load(std::memory_order_seq_cst) == 1) {
            uint32_t expected = 1;
            if (rb->consumer_sleeping.compare_exchange_strong(
                    expected, 0, std::memory_order_acq_rel)) {
                ++(*wakeup_count);
                uint32_t* addr = reinterpret_cast<uint32_t*>(&rb->consumer_sleeping);
                futex_wake(addr, 1);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 4: EVENTFD — blocking read/write on an eventfd
// ─────────────────────────────────────────────────────────────────────────────
#include <sys/eventfd.h>
namespace EventFD {
    static inline void consumer_wait(RingBuffer* rb, WakeupState& ws,
                                     struct io_uring* /*ring*/, uint64_t t) {
        rb->consumer_sleeping.store(1, std::memory_order_seq_cst);
        if (rb->head.load(std::memory_order_seq_cst) != t) {
            uint32_t expected = 1;
            rb->consumer_sleeping.compare_exchange_strong(
                expected, 0, std::memory_order_seq_cst);
            return;
        }
        uint64_t val = 0;
        (void)read(ws.eventfd_fd, &val, sizeof(val));
    }

    static inline void producer_signal(RingBuffer* rb, WakeupState& ws,
                                       struct io_uring* /*ring*/,
                                       uint64_t* wakeup_count) {
        if (rb->consumer_sleeping.load(std::memory_order_seq_cst) == 1) {
            uint32_t expected = 1;
            if (rb->consumer_sleeping.compare_exchange_strong(
                    expected, 0, std::memory_order_acq_rel)) {
                ++(*wakeup_count);
                uint64_t val = 1;
                (void)write(ws.eventfd_fd, &val, sizeof(val));
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 5: IO_URING — FIFO-based wakeup via io_uring (verbatim from existing impl)
// ─────────────────────────────────────────────────────────────────────────────
namespace IoUring {
    static inline void consumer_wait(RingBuffer* rb, WakeupState& ws,
                                     struct io_uring* ring, uint64_t t) {
        rb->consumer_sleeping.store(1, std::memory_order_seq_cst);
        if (rb->head.load(std::memory_order_seq_cst) != t) {
            uint32_t expected = 1;
            rb->consumer_sleeping.compare_exchange_strong(
                expected, 0, std::memory_order_seq_cst);
            return;
        }
        static char sig_buf[8];
        struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
        if (sqe) {
            io_uring_prep_read(sqe, ws.fifo_fd, sig_buf, 1, 0);
            struct io_uring_cqe* cqe;
            if (io_uring_submit_and_wait(ring, 1) >= 0) {
                if (io_uring_peek_cqe(ring, &cqe) == 0)
                    io_uring_cqe_seen(ring, cqe);
            }
        }
    }

    static inline void producer_signal(RingBuffer* rb, WakeupState& ws,
                                       struct io_uring* ring,
                                       uint64_t* wakeup_count) {
        if (rb->consumer_sleeping.load(std::memory_order_seq_cst) == 1) {
            uint32_t expected = 1;
            if (rb->consumer_sleeping.compare_exchange_strong(
                    expected, 0, std::memory_order_acq_rel)) {
                ++(*wakeup_count);
                static char sig = 'W';
                struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
                if (sqe) {
                    io_uring_prep_write(sqe, ws.fifo_fd, &sig, 1, 0);
                    io_uring_submit(ring);
                    struct io_uring_cqe* cqe;
                    if (io_uring_wait_cqe(ring, &cqe) == 0)
                        io_uring_cqe_seen(ring, cqe);
                }
            }
        }
    }
}

#endif // ABLATION_WAKEUP_H
