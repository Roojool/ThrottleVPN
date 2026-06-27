#ifndef RATE_LIMITER_H
#define RATE_LIMITER_H

#include <stdint.h>
#include <pthread.h>

/**
 * Thread-safe token-bucket rate limiter.
 *
 * Tokens represent bytes.  The bucket refills at a steady rate
 * derived from the target bytes-per-second.  Burst capacity is
 * capped at 200 ms worth of tokens so short traffic spikes are
 * absorbed without allowing sustained over-rate.
 *
 * Small packets (<= 128 B, typically ACKs / SYNs) consume half
 * their size in tokens to prioritise latency-sensitive control
 * traffic — a simple form of packet prioritisation that improves
 * gaming and real-time latency.
 */
typedef struct {
    int64_t  tokens;            /* current available tokens (bytes)     */
    int64_t  capacity;          /* maximum burst (20 ms of throughput) */
    int64_t  rate_per_sec;      /* refill rate in bytes / second       */
    int64_t  last_refill_ns;    /* last refill timestamp (CLOCK_MONOTONIC nanos) */
    pthread_mutex_t lock;
} rate_limiter_t;

/** Initialise with a target throughput in bytes/second. */
void rate_limiter_init(rate_limiter_t *rl, int64_t bytes_per_second);

/** Change the target throughput at runtime. */
void rate_limiter_update(rate_limiter_t *rl, int64_t bytes_per_second);

/**
 * Try to consume @p bytes tokens.
 *
 * @param is_small  non-zero if the packet is <= 128 bytes (ACK, SYN, …).
 * @return 0 if tokens were available, otherwise the number of
 *         microseconds the caller should wait before forwarding.
 *         The tokens are consumed regardless — the wait is advisory.
 */
int64_t rate_limiter_consume(rate_limiter_t *rl, int32_t bytes, int is_small);

void rate_limiter_destroy(rate_limiter_t *rl);

#endif /* RATE_LIMITER_H */
