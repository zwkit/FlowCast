package io.github.jqssun.airplay.renderer

import io.github.jqssun.airplay.bridge.NativeBridge
import io.github.jqssun.airplay.viewmodel.AudioDebug

// wrapper around native audio engine
class AudioRenderer {

    @Volatile var config = AudioConfig(); private set

    @Volatile var codecLabel = ""; private set

    private var serverHandle = 0L

    // reused across calls: anchor is polled once per video frame
    private val anchorBuf = LongArray(4)

    @Synchronized
    fun attachEngine(server: Long) {
        serverHandle = server
        pushConfig()
    }

    @Synchronized
    fun detachEngine() {
        serverHandle = 0L
        codecLabel = ""
    }

    // open playout (first onAudioFormat, or resume after pause); idempotent natively
    @Synchronized
    fun start() {
        if (serverHandle == 0L) return
        NativeBridge.nativeServerAudioStart(serverHandle)
    }

    // releases oboe stream + audio device while idle; engine stays alive, freed on server destroy
    @Synchronized
    fun stop() {
        if (serverHandle == 0L) return
        NativeBridge.nativeServerAudioStop(serverHandle)
        codecLabel = ""
    }

    // drop buffered audio + rebuild cushion: content switch / stream discontinuity.
    // used when the video stream signals a discontinuity the audio pts alone
    // doesn't show (mirroring keeps a real-time audio clock across switches)
    @Synchronized
    fun flush() {
        if (serverHandle == 0L) return
        NativeBridge.nativeServerAudioFlush(serverHandle)
    }

    @Synchronized
    fun updateConfig(newConfig: AudioConfig) {
        config = newConfig
        pushConfig()
    }

    private fun pushConfig() {
        if (serverHandle == 0L) return
        NativeBridge.nativeServerAudioConfigure(
            serverHandle, config.cushionMs, config.percentilePct, config.oboeBufferFrames,
            config.forceSwAlac, config.forceSwAac, config.realtimePriority, config.lowLatency,
            config.benchmarkLog)
    }

    private val debugBuf = java.nio.ByteBuffer.allocateDirect(DEBUG_BUF_BYTES)
        .order(java.nio.ByteOrder.LITTLE_ENDIAN)

    // field order/width must mirror native packed AudioDebugData exactly
    @Synchronized
    fun audioDebug(): AudioDebug? {
        if (serverHandle == 0L) return null
        if (!NativeBridge.nativeServerAudioDebug(serverHandle, debugBuf)) return null
        val buf = debugBuf
        buf.rewind()
        val backlogMs = buf.short.toInt() and 0xFFFF
        val tunedCushionMs = buf.short.toInt() and 0xFFFF
        val trims = buf.int
        val drops = buf.int
        val silences = buf.int
        val underruns = buf.int
        val meanUs = buf.int
        val maxUs = buf.int
        val held = buf.short.toInt() and 0xFFFF
        val xrun = buf.int
        val decodeErrors = buf.int
        return AudioDebug(backlogMs, tunedCushionMs, trims, drops, silences, underruns, xrun,
                          meanUs, maxUs, held, decodeErrors)
    }

    @Synchronized
    fun setFormat(ct: Int, spf: Int) {
        codecLabel = when (ct) {
            CT_ALAC -> "ALAC"; CT_AAC_LC -> "AAC-LC"; CT_AAC_ELD -> "AAC-ELD"; else -> "?"
        }
        if (serverHandle != 0L) NativeBridge.nativeServerAudioFormat(serverHandle, ct, spf)
    }

    /**
     * Where the audio playout clock currently is: the pts (in the sender's NTP
     * domain) of the first sample played in the current timeline, the wall clock
     * (System.nanoTime domain) it played at, and an epoch that changes whenever
     * the anchor is re-published (flush, underrun recovery, pause/resume).
     *
     * null = no anchor yet (still priming / flushed / engine detached); callers
     * must then keep whatever clock they had.
     */
    @Synchronized
    fun playoutAnchor(): PlayoutAnchor? {
        if (serverHandle == 0L) return null
        if (!NativeBridge.nativeServerAudioAnchor(serverHandle, anchorBuf)) return null
        return PlayoutAnchor(valid = anchorBuf[0] != 0L, ptsNs = anchorBuf[1],
                             wallNs = anchorBuf[2], epoch = anchorBuf[3].toInt())
    }

    data class PlayoutAnchor(val valid: Boolean, val ptsNs: Long, val wallNs: Long, val epoch: Int)

    companion object {
        // must be >= native sizeof(AudioDebugData) (38 B)
        private const val DEBUG_BUF_BYTES = 64
        const val CT_ALAC = 2
        const val CT_AAC_LC = 4
        const val CT_AAC_ELD = 8
    }
}
