package com.throttlevpn.engine

/**
 * Kotlin-side token bucket used only for speed-test throughput
 * measurement calculations.  The *actual* traffic shaping happens
 * inside the native tun2socks engine via [Tun2SocksEngine.setRateLimit].
 */
class TokenBucket(
    private var rateBytesPerSec: Long,
    burstFactor: Double = 0.2
) {
    private var tokens: Long = 0L
    private var capacity: Long = (rateBytesPerSec * burstFactor).toLong().coerceAtLeast(1500)
    private var lastRefillNanos: Long = System.nanoTime()

    @Synchronized
    fun consume(bytes: Int): Long {
        refill()
        return if (tokens >= bytes) {
            tokens -= bytes
            0L
        } else {
            val deficit = bytes - tokens
            tokens = 0
            if (rateBytesPerSec > 0) (deficit * 1_000_000L) / rateBytesPerSec else 0L
        }
    }

    @Synchronized
    fun updateRate(bytesPerSecond: Long) {
        rateBytesPerSec = bytesPerSecond
        capacity = (bytesPerSecond / 5).coerceAtLeast(1500)
        if (tokens > capacity) tokens = capacity
    }

    private fun refill() {
        val now = System.nanoTime()
        val elapsedMs = (now - lastRefillNanos) / 1_000_000
        if (elapsedMs > 0) {
            tokens += elapsedMs * rateBytesPerSec / 1000
            if (tokens > capacity) tokens = capacity
            lastRefillNanos = now
        }
    }
}
