package com.throttlevpn.speed

import kotlinx.coroutines.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.io.OutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.atomic.AtomicLong
import kotlin.math.max

sealed class SpeedTestState {
    data object Idle : SpeedTestState()
    data class Testing(val phase: String, val progress: Float) : SpeedTestState()
    data class Complete(val result: SpeedTestResult) : SpeedTestState()
    data class Error(val message: String) : SpeedTestState()
}

data class SpeedTestResult(
    val downloadMbps: Double,
    val uploadMbps: Double
)

/**
 * Multi-threaded, time-based bandwidth probe mimicking fast.com/Ookla.
 * Spawns multiple parallel workers connecting to Cloudflare Anycast Edge.
 */
class SpeedTestManager {

    private val _state = MutableStateFlow<SpeedTestState>(SpeedTestState.Idle)
    val state: StateFlow<SpeedTestState> = _state.asStateFlow()

    companion object {
        private const val DL_URL = "https://speed.cloudflare.com/__down?bytes=50000000" // 50MB chunks
        private const val UL_URL = "https://speed.cloudflare.com/__up"
        private const val BUF    = 32_768 // 32KB buffer
        private const val NUM_THREADS = 4
        private const val TEST_DURATION_MS = 8000L
    }

    suspend fun runSpeedTest(): SpeedTestResult = withContext(Dispatchers.IO) {
        try {
            _state.value = SpeedTestState.Testing("Measuring download…", 0f)
            val dl = measureThroughput(isDownload = true)

            _state.value = SpeedTestState.Testing("Measuring upload…", 0.5f)
            val ul = measureThroughput(isDownload = false)

            val result = SpeedTestResult(dl, ul)
            _state.value = SpeedTestState.Complete(result)
            result
        } catch (ex: Exception) {
            val msg = ex.message ?: "Speed test failed"
            if (ex !is CancellationException) {
                _state.value = SpeedTestState.Error(msg)
            }
            SpeedTestResult(0.0, 0.0)
        }
    }

    fun reset() { _state.value = SpeedTestState.Idle }

    /* ---------------------------------------------------------------- */

    private suspend fun measureThroughput(isDownload: Boolean): Double = coroutineScope {
        val totalBytes = AtomicLong(0)
        val startTime = System.nanoTime()

        // Launch worker threads
        val workers = List(NUM_THREADS) {
            launch(Dispatchers.IO) {
                while (isActive) {
                    try {
                        if (isDownload) {
                            runDownloadWorker(totalBytes)
                        } else {
                            runUploadWorker(totalBytes)
                        }
                    } catch (e: Exception) {
                        // Ignore standard IO errors or cancellation during loop
                        if (e is CancellationException) throw e
                    }
                }
            }
        }

        // Ticker for UI updates
        val ticker = launch {
            var lastBytes = 0L
            val prefix = if (isDownload) "Download" else "Upload"
            val baseProgress = if (isDownload) 0f else 0.5f
            
            while (isActive) {
                delay(250)
                val currentBytes = totalBytes.get()
                val elapsedSec = (System.nanoTime() - startTime) / 1_000_000_000.0
                val diffBytes = currentBytes - lastBytes
                lastBytes = currentBytes
                
                // Calculate live Mbps over the last 250ms interval
                val liveMbps = (diffBytes * 8.0) / (0.25 * 1_000_000.0)
                
                val progress = (elapsedSec / (TEST_DURATION_MS / 1000.0)).toFloat().coerceIn(0f, 1f)
                val finalProgress = baseProgress + (progress * 0.5f)
                
                _state.value = SpeedTestState.Testing(
                    "$prefix: %.1f Mbps".format(liveMbps),
                    finalProgress
                )
            }
        }

        // Wait for exact test duration
        delay(TEST_DURATION_MS)
        
        // Stop all workers gracefully
        workers.forEach { it.cancel() }
        ticker.cancel()

        // Calculate final exact average
        val totalSec = (System.nanoTime() - startTime) / 1_000_000_000.0
        val finalMbps = (totalBytes.get() * 8.0) / (totalSec * 1_000_000.0)
        
        max(0.0, finalMbps)
    }

    private fun CoroutineScope.runDownloadWorker(totalBytes: AtomicLong) {
        val conn = (URL(DL_URL).openConnection() as HttpURLConnection).apply {
            connectTimeout = 5000
            readTimeout = 5000
            requestMethod = "GET"
            setRequestProperty("Connection", "keep-alive")
        }
        try {
            conn.connect()
            val input = conn.inputStream
            val buf = ByteArray(BUF)
            while (isActive) {
                val n = input.read(buf)
                if (n == -1) break
                totalBytes.addAndGet(n.toLong())
            }
        } finally {
            conn.disconnect()
        }
    }

    private fun CoroutineScope.runUploadWorker(totalBytes: AtomicLong) {
        // Upload approx 5MB chunk repeatedly
        val chunkSize = 5_000_000
        val conn = (URL(UL_URL).openConnection() as HttpURLConnection).apply {
            connectTimeout = 5000
            readTimeout = 5000
            requestMethod = "POST"
            doOutput = true
            setRequestProperty("Connection", "keep-alive")
            setRequestProperty("Content-Type", "application/octet-stream")
            setFixedLengthStreamingMode(chunkSize)
        }
        try {
            conn.connect()
            val out: OutputStream = conn.outputStream
            val buf = ByteArray(BUF)
            var sent = 0
            while (isActive && sent < chunkSize) {
                val chunk = minOf(BUF, chunkSize - sent)
                out.write(buf, 0, chunk)
                sent += chunk
                totalBytes.addAndGet(chunk.toLong())
            }
            out.flush()
            conn.responseCode // Wait for server ack
        } finally {
            conn.disconnect()
        }
    }
}
