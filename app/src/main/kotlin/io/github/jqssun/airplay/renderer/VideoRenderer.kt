package io.github.jqssun.airplay.renderer

import android.content.Context
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.util.Log
import android.view.Surface
import io.github.jqssun.airplay.renderer.DecoderSelector.Companion.videoCaps
import java.nio.ByteBuffer

class VideoRenderer(ctx: Context) {

    private val lock = Object()
    private val pipeline = VideoPipeline()
    val selector = DecoderSelector(ctx)

    init {
        // pace rendering to the display's refresh rate so a decoder backlog can't inflate the
        // measured fps; defaults to 60Hz when the value is unavailable
        val refreshRate = runCatching {
            (ctx.getSystemService(Context.WINDOW_SERVICE) as? android.view.WindowManager)
                ?.defaultDisplay?.refreshRate ?: 60f
        }.getOrDefault(60f)
        pipeline.setRefreshRate(refreshRate)
        Log.i(TAG, "render pacing refreshRate=${refreshRate}Hz")
    }

    private var avcDecoder: MediaCodecInfo? = null
    private var hevcDecoder: MediaCodecInfo? = null
    private var maxFps = 0
    private var codec: MediaCodec? = null
    private var codecWidth = 0
    private var codecHeight = 0
    private var displaySurface: Surface? = null
    private var currentH265 = false
    private var videoWidth = 0
    private var videoHeight = 0
    private var firstFrameQueued = false

    // stats
    @Volatile var fps = 0; private set
    @Volatile var bitrateBps = 0L; private set
    @Volatile var frameCount = 0L; private set
    @Volatile var codecName = ""; private set
    @Volatile var droppedFrames = 0L; private set
    @Volatile var framePacingJitterUs = 0L; private set

    // video pts jumped (content switch, seek, sender clock re-anchor): the audio
    // stream keeps a real-time clock across these, so its own pts-discontinuity
    // detection never fires and the jitter buffer would play the previous
    // content's tail first. fires from the native video callback thread
    var onVideoDiscontinuity: (() -> Unit)? = null
    private var _lastFedPtsNs = Long.MIN_VALUE

    var enforceSdr = true
    var keyAllowFrameDrop = true
    var scheduledOutputBufferRelease = true
    var benchmarkLog = false
    var benchmarkLogCallback: ((String) -> Unit)? = null

    /**
     * Where the video timeline gets its zero point. When set, the renderer aligns
     * to the audio playout clock (Oboe is the master clock) instead of anchoring
     * to its own first decoded frame — that self-anchoring is what used to leave
     * audio and video each on their own timeline, drifting apart by the audio
     * cushion. Returns null while no anchor exists; the renderer then falls back
     * to self-anchoring and switches over once one shows up.
     */
    var audioAnchorProvider: (() -> AudioRenderer.PlayoutAnchor?)? = null

    // user nudge for TVs whose decoded A/V latency differs from the computed one;
    // positive delays the picture, negative advances it
    @Volatile var avSyncOffsetNs = 0L
    private var _framesThisSec = 0
    private var _bytesThisSec = 0L
    private var _lastStatReset = 0L
    private val _frameIntervalsNs = LongArray(120)
    private var _frameIntervalIdx = 0
    private var _frameIntervalCount = 0
    private var _lastOutputFrameNs = 0L
    // anchors that map decoder PTS (us) to System.nanoTime() for scheduled rendering
    private var _ptsBaseUs = Long.MIN_VALUE
    private var _wallBaseNs = 0L
    // epoch of the audio anchor we aligned to; EPOCH_SELF while self-anchored
    private var _anchorEpoch = Int.MIN_VALUE
    // an anchor epoch proven to be from another clock domain; never align to it again
    private var _rejectedEpoch = Int.MIN_VALUE

    fun setResolution(w: Int, h: Int) {
        videoWidth = w
        videoHeight = h
        pipeline.setVideoSize(w, h)
    }

    // doesn't restart codec; decoder renders into pipeline's own persistent surface
    fun setSurface(surface: Surface) = synchronized(lock) {
        displaySurface = surface
        pipeline.setDisplaySurface(surface)
    }

    fun clearSurface(surface: Surface) = synchronized(lock) {
        if (displaySurface !== surface) return@synchronized
        displaySurface = null
        pipeline.setDisplaySurface(null)
    }

    // wipe the current display surface (black) to clear a stale last frame after a session ends
    fun clearDisplay() = synchronized(lock) {
        pipeline.clearDisplay()
    }

    fun selectDecoders(w: Int, h: Int, fps: Int, h265: Boolean): Boolean = synchronized(lock) {
        avcDecoder = selector.avc()
        hevcDecoder = if (h265) selector.hevc(avcDecoder, w, h, fps) else null
        maxFps = fps
        Log.i(TAG, "decoders: avc=${avcDecoder?.name} hevc=${hevcDecoder?.name}")
        hevcDecoder != null
    }

    // unknown limits default to 1080p
    fun maxResolution(): Pair<Int, Int> =
        listOfNotNull(avcDecoder?.let { it to DecoderSelector.AVC }, hevcDecoder?.let { it to DecoderSelector.HEVC })
            .map { (info, mime) ->
                runCatching { info.videoCaps(mime).let { it.supportedWidths.upper to it.supportedHeights.upper } }
                    .getOrDefault(1920 to 1080)
            }
            .reduceOrNull { (w1, h1), (w2, h2) -> minOf(w1, w2) to minOf(h1, h2) } ?: (1920 to 1080)

    // codec per mirror session; pipeline persists across sessions
    fun startSession() = synchronized(lock) { _resetStats() }

    fun stopSession() = synchronized(lock) { stopCodec() }

    private fun _resetStats() {
        fps = 0; bitrateBps = 0; frameCount = 0; codecName = ""
        droppedFrames = 0; framePacingJitterUs = 0
        _framesThisSec = 0; _bytesThisSec = 0
    }

    private fun _updateStats(size: Int) {
        val now = System.currentTimeMillis()
        if (now - _lastStatReset >= 1000) {
            // report the frame rate actually rendered on screen, not the network-arrival rate;
            // input frames may arrive at 60fps while drops/hiccups mean far fewer are displayed.
            fps = pipeline.renderFps
            bitrateBps = _bytesThisSec * 8
            framePacingJitterUs = _computeFramePacingJitterUs()
            _framesThisSec = 0
            _bytesThisSec = 0
            _lastStatReset = now
            if (benchmarkLog) _emitBenchmarkLine()
        }
        _framesThisSec++
        _bytesThisSec += size
        frameCount++
    }

    private fun _emitBenchmarkLine() {
        val msg = "fps=$fps bitrate=${bitrateBps / 1000}kbps " +
            "jitter=${framePacingJitterUs}us frames=$frameCount " +
            "dropped=$droppedFrames codec=$codecName " +
            "res=${videoWidth}x${videoHeight}"
        Log.i(BENCH_TAG, msg)
        benchmarkLogCallback?.invoke(msg)
    }

    fun feedFrame(buf: ByteBuffer, len: Int, ntpTimeNs: Long, isH265: Boolean) {
        synchronized(lock) {
            _updateStats(len)

            // pts discontinuity: new content (switch/seek) or a sender clock
            // re-anchor. signal the audio side to drop its old timeline — mirror
            // audio pts stays continuous across switches, so this video-side
            // signal is often the only warning the new content gets
            if (_lastFedPtsNs != Long.MIN_VALUE &&
                Math.abs(ntpTimeNs - _lastFedPtsNs) > VIDEO_PTS_JUMP_NS) {
                Log.i(TAG, String.format("video pts jump: %.2fs -> %.2fs, flushing audio timeline",
                      _lastFedPtsNs / 1e9, ntpTimeNs / 1e9))
                onVideoDiscontinuity?.invoke()
            }
            _lastFedPtsNs = ntpTimeNs

            if (videoWidth == 0 || videoHeight == 0) return

            if (codec == null || isH265 != currentH265 ||
                videoWidth != codecWidth || videoHeight != codecHeight) {
                // codec was sized for a previous video/frame geometry (e.g. phone rotated from
                // portrait to landscape); it must be rebuilt at the new size or the decoder
                // misinterprets the new frames and the picture freezes. Wait for a keyframe
                // (new SPS carries the dimensions) so the rebuilt decoder starts clean.
                if (!_isKeyframe(buf, len, isH265)) {
                    if (codec != null) stopCodec()
                    return
                }
                stopCodec()
            }

            try {
                if (codec == null) startCodec(isH265)
                _feedToCodec(buf, len, ntpTimeNs)
                drainOutput()
            } catch (e: Exception) {
                Log.w(TAG, "Codec error, resetting", e)
                stopCodec()
            }
        }
    }

    private fun _feedToCodec(buf: ByteBuffer, len: Int, ntpTimeNs: Long) {
        val c = codec ?: return
        // dropping a frame desyncs decoder until the next keyframe, but source would only send one on (re)connect
        val retries = if (firstFrameQueued) FEED_RETRIES else FIRST_FEED_RETRIES
        repeat(retries) {
            val idx = c.dequeueInputBuffer(FEED_WAIT_US)
            if (idx >= 0) {
                val inBuf = c.getInputBuffer(idx) ?: return
                inBuf.clear()
                buf.position(0)
                buf.limit(len)
                inBuf.put(buf)
                c.queueInputBuffer(idx, 0, len, ntpTimeNs / 1000, 0)
                firstFrameQueued = true
                return
            }
            drainOutput()
        }
        droppedFrames++
        Log.w(TAG, "Decoder input queue full; dropping frame. drops=$droppedFrames")
    }

    private fun _isKeyframe(buf: ByteBuffer, len: Int, isH265: Boolean): Boolean {
        if (len < 5) return false
        var i = 0
        while (i <= len - 5) {
            if (buf.get(i) == 0.toByte() && buf.get(i + 1) == 0.toByte() &&
                buf.get(i + 2) == 0.toByte() && buf.get(i + 3) == 1.toByte()) {
                val key = if (isH265) {
                    val type = (buf.get(i + 4).toInt() shr 1) and 0x3F
                    type == 19 || type == 20 || type == 21 || type == 32 || type == 33
                } else {
                    val type = buf.get(i + 4).toInt() and 0x1F
                    type == 5 || type == 7
                }
                if (key) return true
            }
            i++
        }
        return false
    }

    private fun startCodec(h265: Boolean) {
        codecWidth = videoWidth
        codecHeight = videoHeight
        pipeline.start()
        pipeline.setVideoSize(videoWidth, videoHeight)
        val s = pipeline.inputSurface ?: return
        currentH265 = h265
        val mime = if (h265) DecoderSelector.HEVC else DecoderSelector.AVC
        val info = (if (h265) hevcDecoder else avcDecoder) ?: error("no decoder selected for $mime")

        firstFrameQueued = false
        try {
            _startWithLadder(info, mime, s, h265)
        } catch (e: Exception) {
            // strict hw decoders reject configs beyond their real limits
            val sw = selector.software(mime, videoWidth, videoHeight) ?: throw e
            Log.w(TAG, "Hardware decoder failed, trying software fallback", e)
            _startWithLadder(sw, mime, s, h265)
        }
        Log.i(TAG, "Video codec started: $mime ${videoWidth}x${videoHeight} ($codecName)")
    }

    private fun _startWithLadder(info: MediaCodecInfo, mime: String, s: Surface, h265: Boolean) {
        var tryNum = 0
        while (true) {
            val format = _format(mime, info)
            val more = selector.lowLatencyOptions(format, info, mime, tryNum)
            try {
                _startDecoder(MediaCodec.createByCodecName(info.name), format, s, h265)
                return
            } catch (e: Exception) {
                if (!more) throw e
                Log.w(TAG, "configure try $tryNum failed: $format", e)
                tryNum++
            }
        }
    }

    private fun _format(mime: String, info: MediaCodecInfo) = MediaFormat.createVideoFormat(mime, videoWidth, videoHeight).apply {
        setInteger(MediaFormat.KEY_FRAME_RATE, maxFps)
        if (selector.adaptive(info, mime)) {
            setInteger(MediaFormat.KEY_MAX_WIDTH, videoWidth)
            setInteger(MediaFormat.KEY_MAX_HEIGHT, videoHeight)
        }
        setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, maxOf(videoWidth * videoHeight * 3 / 4, 1024 * 1024))
        if (enforceSdr) {
            setInteger(MediaFormat.KEY_COLOR_STANDARD, MediaFormat.COLOR_STANDARD_BT709)
            setInteger(MediaFormat.KEY_COLOR_RANGE, MediaFormat.COLOR_RANGE_LIMITED)
            setInteger(MediaFormat.KEY_COLOR_TRANSFER, MediaFormat.COLOR_TRANSFER_SDR_VIDEO)
        }
        if (android.os.Build.VERSION.SDK_INT >= 29) {
            setInteger(MediaFormat.KEY_ALLOW_FRAME_DROP, if (keyAllowFrameDrop) 1 else 0)
        }
    }

    private fun _startDecoder(c: MediaCodec, format: MediaFormat, surface: Surface, h265: Boolean) {
        try {
            c.configure(format, surface, null, 0)
            c.start()
        } catch (e: Exception) {
            try { c.release() } catch (_: Exception) {}
            throw e
        }
        codec = c
        codecName = (if (h265) "H.265" else "H.264") + " (${c.name})"
    }

    private fun stopCodec() {
        _frameIntervalIdx = 0
        _frameIntervalCount = 0
        _lastOutputFrameNs = 0L
        _ptsBaseUs = Long.MIN_VALUE
        _wallBaseNs = 0L
        _anchorEpoch = Int.MIN_VALUE
        _rejectedEpoch = Int.MIN_VALUE
        _lastFedPtsNs = Long.MIN_VALUE
        codec?.let {
            try {
                it.stop()
                it.release()
            } catch (_: Exception) {}
        }
        codec = null
        codecWidth = 0
        codecHeight = 0
    }

    private fun drainOutput() {
        val c = codec ?: return
        val info = MediaCodec.BufferInfo()
        while (true) {
            val idx = c.dequeueOutputBuffer(info, 0)
            if (idx < 0) break
            _recordOutputFrameTime()
            if (scheduledOutputBufferRelease) {
                // schedule frame at VSYNC matching its NTP presentation time
                val ptsUs = info.presentationTimeUs
                _ensureAnchor(ptsUs)
                val now = System.nanoTime()
                var renderNs = scheduledRenderNs(_ptsBaseUs, _wallBaseNs, ptsUs, avSyncOffsetNs)
                if (leadsTooFar(renderNs, now)) {
                    // the anchor cannot describe this stream — e.g. a leftover audio
                    // timeline from an earlier session, or video whose pts comes from
                    // a different clock domain (e.g. an MPEG-TS stream). honouring it would park
                    // the frame in the future and freeze the picture, so self-anchor
                    // instead and remember the epoch so we stop retrying it on every
                    // frame; a later re-prime (new epoch) gets another chance
                    _rejectedEpoch = _anchorEpoch
                    _selfAnchor(ptsUs, now)
                    if (_rejectedEpoch != EPOCH_SELF) {
                        Log.w(TAG, "ignoring audio anchor epoch=$_rejectedEpoch: it maps this stream outside the present")
                    }
                    renderNs = now
                }
                c.releaseOutputBuffer(idx, renderNs)
            } else {
                c.releaseOutputBuffer(idx, true)
            }
        }
    }

    /**
     * Pins the video timeline to the audio playout clock when one is available.
     * The audio engine re-publishes its anchor on every (re)prime (stream start,
     * flush, underrun recovery, resume); a changed epoch means we re-align, which
     * also cancels any drift accumulated since the last anchor. With no audio at
     * all we anchor to our own first decoded frame and stay there.
     */
    private fun _ensureAnchor(ptsUs: Long) {
        val anchor = audioAnchorProvider?.invoke()
        if (anchor != null && anchor.valid && anchor.epoch != _anchorEpoch && anchor.epoch != _rejectedEpoch) {
            _ptsBaseUs = anchor.ptsNs / 1000  // anchor pts is ns, decoder pts is us
            _wallBaseNs = anchor.wallNs
            _anchorEpoch = anchor.epoch
            Log.i(TAG, "video timeline aligned to audio clock (epoch=$_anchorEpoch)")
        } else if (_ptsBaseUs == Long.MIN_VALUE) {
            _selfAnchor(ptsUs, System.nanoTime())
        }
    }

    /** Drops any audio alignment: the first frame becomes the zero point again. */
    private fun _selfAnchor(ptsUs: Long, nowNs: Long) {
        _ptsBaseUs = ptsUs
        _wallBaseNs = nowNs
        _anchorEpoch = EPOCH_SELF
    }

    fun release() = synchronized(lock) {
        stopCodec()
        pipeline.release()
        _resetStats()
    }

    private fun _recordOutputFrameTime() {
        val now = System.nanoTime()
        if (_lastOutputFrameNs > 0) {
            _frameIntervalsNs[_frameIntervalIdx % _frameIntervalsNs.size] = now - _lastOutputFrameNs
            _frameIntervalIdx++
            _frameIntervalCount++
        }
        _lastOutputFrameNs = now
    }

    private fun _computeFramePacingJitterUs(): Long {
        val count = _frameIntervalCount.coerceAtMost(_frameIntervalsNs.size)
        if (count < 2) return 0

        var sum = 0.0
        var sumSq = 0.0
        for (i in 0 until count) {
            val interval = _frameIntervalsNs[i].toDouble()
            sum += interval
            sumSq += interval * interval
        }
        val mean = sum / count
        val variance = (sumSq / count) - (mean * mean)
        return (kotlin.math.sqrt(variance.coerceAtLeast(0.0)) / 1000.0).toLong()
    }

    companion object {
        private const val TAG = "VideoRenderer"
        private const val BENCH_TAG = "BENCHMARK"
        private const val FEED_WAIT_US = 20_000L
        private const val FEED_RETRIES = 10
        private const val FIRST_FEED_RETRIES = 50

        // marks "we anchored to our own first frame" so a later audio anchor (epoch
        // starts at 0 and only grows) still takes over when audio shows up
        internal const val EPOCH_SELF = Int.MAX_VALUE

        // video pts jump larger than this = content switch / seek / clock
        // re-anchor rather than normal frame pacing (16.7ms at 60fps)
        private const val VIDEO_PTS_JUMP_NS = 1_000_000_000L

        /**
         * Wall-clock instant (System.nanoTime domain) at which a frame with pts
         * [framePtsUs] should hit the screen, given the anchor that maps
         * [anchorPtsUs] to [anchorWallNs] and a user [offsetNs] nudge.
         * Pure so the math is unit-tested; drainOutput() calls it per frame.
         */
        internal fun scheduledRenderNs(
            anchorPtsUs: Long,
            anchorWallNs: Long,
            framePtsUs: Long,
            offsetNs: Long,
        ): Long = anchorWallNs + offsetNs + (framePtsUs - anchorPtsUs) * 1000L

        // a frame may legitimately be scheduled ahead of now (decode-ahead plus the
        // audio cushion, both well under this); more than that means the anchor does
        // not describe this stream and scheduling it would freeze the picture
        internal const val MAX_SCHEDULE_LEAD_NS = 2_000_000_000L

        /** True when [renderNs] is so far ahead of [nowNs] that the anchor must be bogus. */
        internal fun leadsTooFar(renderNs: Long, nowNs: Long): Boolean =
            renderNs - nowNs > MAX_SCHEDULE_LEAD_NS
    }
}
