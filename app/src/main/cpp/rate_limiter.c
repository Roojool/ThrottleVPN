#include "rate_limiter.h"

#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

static int64_t now_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

void rate_limiter_init(rate_limiter_t *rl, int64_t bytes_per_second) {
    memset(rl, 0, sizeof(*rl));
    pthread_mutex_init(&rl->lock, NULL);

    rl->rate_per_sec  = bytes_per_second;
    rl->capacity      = bytes_per_second / 50;         /* 20 ms burst */
    if (rl->capacity < 1500) rl->capacity = 1500;      /* at least 1 MTU */
    rl->tokens        = rl->capacity;
    rl->last_refill_ns = now_monotonic_ns();
}

void rate_limiter_update(rate_limiter_t *rl, int64_t bytes_per_second) {
    pthread_mutex_lock(&rl->lock);

    rl->rate_per_sec = bytes_per_second;
    rl->capacity     = bytes_per_second / 50;
    if (rl->capacity < 1500) rl->capacity = 1500;
    if (rl->tokens > rl->capacity) {
        rl->tokens = rl->capacity;
    }

    pthread_mutex_unlock(&rl->lock);
}

int64_t rate_limiter_consume(rate_limiter_t *rl, int32_t bytes, int is_small) {
    pthread_mutex_lock(&rl->lock);

    /* ---- refill -------------------------------------------------- */
    int64_t now = now_monotonic_ns();
    int64_t elapsed_ns = now - rl->last_refill_ns;
    if (elapsed_ns > 0 && rl->rate_per_sec > 0) {
        int64_t added = (elapsed_ns * rl->rate_per_sec) / 1000000000LL;
        if (added > 0) {
            rl->tokens += added;
            if (rl->tokens >= rl->capacity) {
                rl->tokens = rl->capacity;
                rl->last_refill_ns = now;  /* Reset to prevent delayed burst build-up */
            } else {
                rl->last_refill_ns += (added * 1000000000LL) / rl->rate_per_sec;
            }
        }
    }

    /* ---- cost (small-packet discount) ----------------------------- */
    int32_t cost = is_small ? (bytes / 2 + 1) : bytes;

    /* ---- consume -------------------------------------------------- */
    if (rl->tokens >= cost) {
        rl->tokens -= cost;
        pthread_mutex_unlock(&rl->lock);
        return 0;                           /* immediate pass */
    }

    int64_t deficit  = cost - rl->tokens;
    int64_t wait_us  = 0;
    if (rl->rate_per_sec > 0) {
        wait_us = (deficit * 1000000LL) / rl->rate_per_sec;   /* µs */
    }

    rl->tokens -= cost;                     /* go negative — caller waits */

    pthread_mutex_unlock(&rl->lock);

    /* Batch sleep to avoid OS scheduling overhead/jitter. */
    if (wait_us < 2000) return 0;

    /* Cap advisory wait to 50 ms to keep the event loop responsive. */
    if (wait_us > 50000) wait_us = 50000;
    return wait_us;
}

void rate_limiter_destroy(rate_limiter_t *rl) {
    pthread_mutex_destroy(&rl->lock);
}
