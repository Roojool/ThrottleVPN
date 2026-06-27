package com.throttlevpn.engine

import android.net.VpnService

/**
 * Kotlin-side wrapper around the native tun2socks library.
 *
 * The native engine runs its own pthread with an epoll event loop that
 * reads/writes packets from the TUN file descriptor.  Rate-limiting is
 * applied in C via two token-bucket shapers (one per direction).
 *
 * The native code calls back into [protectSocket] from its thread to
 * prevent newly-created relay sockets from being re-routed through the
 * VPN tunnel (infinite-loop prevention).
 */
class Tun2SocksEngine(private val vpnService: VpnService) {

    companion object {
        init {
            System.loadLibrary("tun2socks")
        }
    }

    /* ------ JNI declarations ---------------------------------------- */

    private external fun nativeStart(tunFd: Int, mtu: Int): Int
    private external fun nativeStop()
    private external fun nativeSetRateLimit(downloadBps: Long, uploadBps: Long)
    private external fun nativeGetStats(): LongArray

    /* ------ Callback from native (invoked on the engine pthread) ---- */

    /**
     * Called from native code to protect a socket fd so the system does
     * not route its traffic back through the TUN interface.
     */
    @Suppress("unused")
    fun protectSocket(fd: Int): Boolean = vpnService.protect(fd)

    /* ------ Public Kotlin API --------------------------------------- */

    fun start(tunFd: Int, mtu: Int = 1500): Boolean = nativeStart(tunFd, mtu) == 0

    fun stop() = nativeStop()

    /**
     * Update the per-direction bandwidth caps.
     *
     * @param downloadMbps maximum download rate in megabits / second.
     * @param uploadMbps   maximum upload   rate in megabits / second.
     */
    fun setRateLimit(downloadMbps: Double, uploadMbps: Double) {
        val dlBytes = (downloadMbps * 1_000_000.0 / 8.0).toLong().coerceAtLeast(0)
        val ulBytes = (uploadMbps   * 1_000_000.0 / 8.0).toLong().coerceAtLeast(0)
        nativeSetRateLimit(dlBytes, ulBytes)
    }

    /** Returns total bytes transferred since engine start. */
    fun getStats(): TrafficStats {
        val arr = nativeGetStats()
        return TrafficStats(uploadBytes = arr[0], downloadBytes = arr[1])
    }
}

data class TrafficStats(
    val uploadBytes: Long,
    val downloadBytes: Long
)
