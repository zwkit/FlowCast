package io.github.jqssun.airplay.renderer

import android.content.Context
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.view.Surface
import androidx.media3.common.C
import androidx.media3.common.Format
import androidx.media3.common.MediaItem
import androidx.media3.common.MediaMetadata
import androidx.media3.common.MimeTypes
import androidx.media3.common.PlaybackParameters
import androidx.media3.common.Player
import androidx.media3.common.Timeline
import androidx.media3.common.Tracks
import androidx.media3.common.VideoSize
import androidx.media3.datasource.DefaultHttpDataSource
import androidx.media3.exoplayer.DefaultLoadControl
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.exoplayer.analytics.AnalyticsListener
import androidx.media3.exoplayer.rtsp.RtspMediaSource
import androidx.media3.exoplayer.source.DefaultMediaSourceFactory
import androidx.media3.exoplayer.source.MediaSource
import androidx.media3.exoplayer.source.ProgressiveMediaSource
import androidx.media3.extractor.ExtractorsFactory
import androidx.media3.extractor.flv.FlvExtractor
import androidx.media3.extractor.ts.TsExtractor
import io.github.jqssun.airplay.StreamUrl
import io.github.jqssun.airplay.stream.RtmpDataSource
import io.github.jqssun.airplay.stream.UdpStreamDataSource
import kotlin.math.roundToInt

// rate is 0 while buffering; speed is the configured rate regardless of pause state
// native reads the effective rate, overlay reads playWhenReady + speed + skipSilence
data class PlaybackSnapshot(
    val position: Float,
    val duration: Float,
    val rate: Float,
    val ready: Boolean,
    val playWhenReady: Boolean,
    val speed: Float = 1f,
    val skipSilence: Boolean = false,
    val buffering: Boolean = false,
)

// selected video track metadata, surfaced to the debug overlay and the player's info line
data class VideoFormatInfo(
    val codecs: String = "",
    val width: Int = 0,
    val height: Int = 0,
    val bitrateBps: Long = 0,
    // frame rate declared by the container; 0 for most progressive mp4s, which do not carry it
    val declaredFps: Float = 0f,
    // render rate measured from the decoder's output, used when the container is silent
    val measuredFps: Float = 0f,
) {
    val fps: Float get() = if (declaredFps > 0f) declaredFps else measuredFps
}

// exoplayer calls stay on the main thread; native only reads the onPlaybackInfo snapshot
@androidx.annotation.OptIn(androidx.media3.common.util.UnstableApi::class)
class AirPlayVideoPlayer(private val context: Context) {

    private val mainHandler = Handler(Looper.getMainLooper())
    private var player: ExoPlayer? = null
    private var pendingSurface: Surface? = null

    // latest snapshot, readable from any thread (DLNA GetPositionInfo etc.)
    @Volatile private var _snapshot = PlaybackSnapshot(0f, 0f, 0f, false, false)

    // selected video track metadata for the debug overlay / player info line (mirror renderer
    // does not cover this path)
    @Volatile private var _videoFormatInfo = VideoFormatInfo()

    var onPlaybackInfo: ((PlaybackSnapshot) -> Unit)? = null
    var onVideoSize: ((width: Int, height: Int, aspect: Float) -> Unit)? = null
    // fired when the selected track changes or the measured frame rate settles on a new value
    var onVideoFormat: ((VideoFormatInfo) -> Unit)? = null
    var onTitle: ((String?) -> Unit)? = null
    var onEnded: (() -> Unit)? = null

    // ---- frame-rate measurement ------------------------------------------------------------
    // Most progressive mp4s carry no frame rate in their container (Format.frameRate == 0), so
    // the info line would always read "0 fps". MediaCodec reports a running frame count, so the
    // render rate is measured over 1 s windows instead.
    @Volatile private var _speed = 1f
    private var _frameSampleAtMs = 0L
    private var _frameSampleCount = 0
    @Volatile private var _measuredFps = 0f

    private val _analytics = object : AnalyticsListener {
        override fun onVideoFrameProcessingOffset(
            eventTime: AnalyticsListener.EventTime,
            totalProcessingOffsetUs: Long,
            frameCount: Int,
        ) {
            // called on the playback thread -> only touch plain fields, never the player
            val now = SystemClock.elapsedRealtime()
            val lastAt = _frameSampleAtMs
            if (lastAt == 0L) {
                _frameSampleAtMs = now
                _frameSampleCount = frameCount
                return
            }
            val dt = now - lastAt
            if (dt < 1_000) return
            val frames = frameCount - _frameSampleCount
            _frameSampleAtMs = now
            _frameSampleCount = frameCount
            // a sped-up stream renders faster than its source rate; normalise it back
            val fps = frames * 1000f / dt / _speed.coerceAtLeast(0.1f)
            if (fps in 5f..240f) _measuredFps = fps
        }
    }

    /**
     * The selected video track's [Format], taken from the [Tracks] snapshot itself. Returns null
     * while the renderer has not selected anything yet (onTracksChanged fires early and often).
     */
    private fun _selectedVideoFormat(tracks: Tracks): Format? {
        for (group in tracks.groups) {
            if (group.type != C.TRACK_TYPE_VIDEO || !group.isSelected) continue
            for (i in 0 until group.length) {
                val f = group.getTrackFormat(i)
                if (f.height > 0) return f
            }
        }
        return null
    }

    private val _reportTick = object : Runnable {
        override fun run() {
            _reportPlaybackInfo()
            mainHandler.postDelayed(this, REPORT_INTERVAL_MS)
        }
    }

    @Volatile private var _lastLocation: String? = null
    // an RTSP stream is retried exactly once over RTP/TCP: media3 has the same idea built in
    // (RtspMediaPeriod.retryWithRtpTcp) but on 461 it only records the error, so a server that
    // refuses UDP — every one of them behind a firewall/proxy that does not forward the extra
    // RTP ports — just fails. Measured against MediaMTX: two SETUPs, both client_port=…, then
    // "SETUP 461" with no reconnect at all.
    private var _rtspRetriedTcp = false

    private val _listener = object : Player.Listener {
        override fun onPlayerError(error: androidx.media3.common.PlaybackException) {
            val location = _lastLocation
            if (location != null && StreamUrl.isRtsp(location) && !_rtspRetriedTcp) {
                Log.w(TAG, "rtsp failed (${error.errorCodeName}), retrying once over RTP/TCP")
                _rtspRetriedTcp = true
                // post instead of calling straight through: the old player is released inside
                // _playInternal, which must not happen on the callback's stack
                mainHandler.post { _playInternal(location, 0f, forceRtpTcp = true) }
                return
            }
            Log.w(TAG, "playback error", error)
            Log.e(TAG, "playback failed uri=$location code=${error.errorCodeName} msg=${error.message} cause=${error.cause}")
            onEnded?.invoke()
        }
        override fun onPlaybackStateChanged(state: Int) {
            if (state == Player.STATE_ENDED) onEnded?.invoke()
        }
        override fun onTimelineChanged(timeline: Timeline, reason: Int) {
            // duration is usually established here (esp. hls): report so the held /play releases
            _reportPlaybackInfo()
        }
        override fun onTracksChanged(tracks: Tracks) {
            // read the format off the Tracks handed to us: player.videoFormat is still null while
            // this callback runs (measured on an emulator: it logged "nullxnull" even though the
            // video was already decoding), so keying the info line off it left it blank forever
            val f = _selectedVideoFormat(tracks) ?: player?.videoFormat?.takeIf { it.height > 0 }
            Log.i(TAG, "tracks changed: videoFormat=${f?.width}x${f?.height} codecs=${f?.codecs} " +
                "mime=${f?.sampleMimeType} fps=${f?.frameRate}")
            _videoFormatInfo = if (f != null && f.height > 0) {
                VideoFormatInfo(
                    codecs = f.codecs ?: f.sampleMimeType ?: "",
                    width = f.width,
                    height = f.height,
                    bitrateBps = f.averageBitrate.toLong(),
                    declaredFps = if (f.frameRate.isFinite() && f.frameRate > 0f) f.frameRate else 0f,
                    measuredFps = _videoFormatInfo.measuredFps,
                )
            } else {
                VideoFormatInfo()
            }
            onVideoFormat?.invoke(_videoFormatInfo)
        }
        override fun onMediaMetadataChanged(mediaMetadata: MediaMetadata) {
            onTitle?.invoke(mediaMetadata.title?.toString())
        }
        override fun onVideoSizeChanged(videoSize: VideoSize) {
            if (videoSize.height == 0) return
            val width = (videoSize.width * videoSize.pixelWidthHeightRatio).toInt()
            onVideoSize?.invoke(width, videoSize.height, width.toFloat() / videoSize.height)
            // the info line must not stay blank when the track's Format carried no resolution
            // (or onTracksChanged ran before the decoder settled): fill it from the real size
            if (_videoFormatInfo.width != width || _videoFormatInfo.height != videoSize.height) {
                _videoFormatInfo = _videoFormatInfo.copy(width = width, height = videoSize.height)
                onVideoFormat?.invoke(_videoFormatInfo)
            }
        }
    }

    fun play(location: String, startPositionSeconds: Float) = mainHandler.post {
        _rtspRetriedTcp = false
        _playInternal(location, startPositionSeconds, forceRtpTcp = false)
    }

    private fun _playInternal(location: String, startPositionSeconds: Float, forceRtpTcp: Boolean) {
        _lastLocation = location
        // recycling must not report the stopped sentinel: senders poll right after /play
        _stopInternal(reportStopped = false)
        // browser-like UA + per-host Referer so DRM/CDN-backed video streams don't reject us
        val http = DefaultHttpDataSource.Factory()
            .setUserAgent(USER_AGENT)
            .setAllowCrossProtocolRedirects(true)
            .setConnectTimeoutMs(20000)
            .setReadTimeoutMs(20000)
            .setDefaultRequestProperties(refererHeaders(location))
        // larger buffers smooth out weak decoders / flaky CDN segments on TVs
        val loadControl = DefaultLoadControl.Builder()
            .setBufferDurationsMs(20000, 40000, 1500, 5000)
            .build()
        // rtsp:// reaches ExoPlayer as a bare Uri and DefaultMediaSourceFactory has no branch for
        // it (DASH/HLS/SmoothStreaming/progressive only), so a pushed camera URL used to fail
        // with a container-unsupported error. RtspMediaSource brings its own RTSP client + RTP
        // readers. forceRtpTcp only on the retry, see _listener.onPlayerError.
        //
        // rtmp:// and udp/rtp:// are built the other way round: neither carries a container, so
        // each gets a DataSource that turns the wire format into one media3 can demux — FLV for
        // RTMP (its messages are FLV tags), MPEG-TS for datagrams.
        val kind = StreamUrl.kindOf(location)
        val sourceFactory: MediaSource.Factory = when (kind) {
            StreamUrl.Kind.RTSP -> RtspMediaSource.Factory()
                .setUserAgent(USER_AGENT)
                .setForceUseRtpTcp(forceRtpTcp)
            StreamUrl.Kind.RTMP -> ProgressiveMediaSource.Factory(
                RtmpDataSource.Factory(),
                ExtractorsFactory { arrayOf(FlvExtractor()) },
            )
            StreamUrl.Kind.UDP, StreamUrl.Kind.RTP -> ProgressiveMediaSource.Factory(
                UdpStreamDataSource.Factory(kind == StreamUrl.Kind.RTP, context.applicationContext),
                ExtractorsFactory { arrayOf(TsExtractor()) },
            )
            else -> DefaultMediaSourceFactory(context).setDataSourceFactory(http)
        }
        val p = ExoPlayer.Builder(context)
            .setMediaSourceFactory(sourceFactory)
            .setLoadControl(loadControl)
            .build().also {
                it.addListener(_listener)
                it.addAnalyticsListener(_analytics)
                pendingSurface?.let { s -> it.setVideoSurface(s) }
            }
        // a new media item restarts the frame-rate measurement
        _videoFormatInfo = VideoFormatInfo()
        _frameSampleAtMs = 0L
        _frameSampleCount = 0
        _measuredFps = 0f
        onVideoFormat?.invoke(_videoFormatInfo)
        player = p
        // Youku/QiYi-style DLNA sends an HLS URL ("/playlist/m3u8") that the progressive-content
        // sniffing can fail to recognize -> ERROR_CODE_PARSING_CONTAINER_UNSUPPORTED (black screen).
        // Nudge ExoPlayer onto the HLS path explicitly when the URI looks like HLS (the same
        // trick a receiver's createMediaItem uses).
        val item = when {
            // a datagram/RTMP stream never ends: without the live hint the player fills up to
            // maxBufferMs (40s here) before it starts, and never catches up to the live edge
            kind == StreamUrl.Kind.RTMP || kind == StreamUrl.Kind.UDP || kind == StreamUrl.Kind.RTP ->
                MediaItem.Builder().setUri(location)
                    .setLiveConfiguration(
                        MediaItem.LiveConfiguration.Builder()
                            .setTargetOffsetMs(LIVE_TARGET_OFFSET_MS)
                            .setMaxPlaybackSpeed(1.02f)
                            .build(),
                    )
                    .build()
            isHlsUri(location) ->
                MediaItem.Builder().setUri(location).setMimeType(MimeTypes.APPLICATION_M3U8).build()
            else -> MediaItem.fromUri(location)
        }
        p.setMediaItem(item, (startPositionSeconds * 1000).toLong())
        p.playWhenReady = true
        p.prepare()
        onPlaybackInfo?.invoke(PlaybackSnapshot(startPositionSeconds, 0f, 0f, false, true))
        mainHandler.postDelayed(_reportTick, REPORT_INTERVAL_MS)
    }

    fun scrub(positionSeconds: Float) = mainHandler.post {
        player?.seekTo((positionSeconds * 1000).toLong())
    }

    // coalesces the seek spam from drag-seeking
    @androidx.annotation.OptIn(androidx.media3.common.util.UnstableApi::class)
    fun setScrubbing(enabled: Boolean) = mainHandler.post {
        player?.isScrubbingModeEnabled = enabled
    }

    fun setRate(rate: Float) = mainHandler.post {
        val p = player ?: return@post
        if (rate <= 0f) {
            p.playWhenReady = false
        } else {
            p.playbackParameters = PlaybackParameters(rate.coerceAtLeast(0.1f))
            p.playWhenReady = true
        }
    }

    // local-only: the sender self-syncs from its next /playback-info poll
    fun setPlaying(playing: Boolean) = mainHandler.post {
        player?.playWhenReady = playing
    }

    // local speed toggle: unlike setRate it must not resume a paused player
    fun setSpeed(speed: Float) = mainHandler.post {
        player?.playbackParameters = PlaybackParameters(speed.coerceAtLeast(0.1f))
    }

    fun setSkipSilence(enabled: Boolean) = mainHandler.post {
        player?.skipSilenceEnabled = enabled
    }

    fun seekBy(deltaMs: Long) = mainHandler.post {
        val p = player ?: return@post
        var target = (p.currentPosition + deltaMs).coerceAtLeast(0)
        val duration = p.duration
        if (duration != C.TIME_UNSET) {
            target = target.coerceAtMost(duration)
        }
        p.seekTo(target)
    }

    fun setSurface(surface: Surface) = mainHandler.post {
        pendingSurface = surface
        player?.setVideoSurface(surface)
    }

    // no-op if a newer surface already replaced this one
    fun clearSurface(surface: Surface) = mainHandler.post {
        if (pendingSurface !== surface) return@post
        pendingSurface = null
        player?.setVideoSurface(null)
    }

    fun stop() = mainHandler.post { _stopInternal(reportStopped = true) }

    private fun _stopInternal(reportStopped: Boolean) {
        mainHandler.removeCallbacks(_reportTick)
        player?.let {
            it.removeListener(_listener)
            it.release()
        }
        player = null
        _speed = 1f
        _frameSampleAtMs = 0L
        _frameSampleCount = 0
        _measuredFps = 0f
        if (_videoFormatInfo != VideoFormatInfo()) {
            _videoFormatInfo = VideoFormatInfo()
            onVideoFormat?.invoke(_videoFormatInfo)
        }
        // duration=-1 is the "video finished" sentinel for the playback-info handler
        if (reportStopped) {
            onPlaybackInfo?.invoke(PlaybackSnapshot(0f, -1f, 0f, false, false))
        }
    }

    private fun _reportPlaybackInfo() {
        val p = player ?: return
        val durationMs = p.duration
        val position = p.currentPosition / 1000f
        // 0 = live/unknown (TIME_UNSET); a real value only exists for vod
        val duration = if (durationMs == C.TIME_UNSET) 0f else durationMs / 1000f
        val rate = if (p.playWhenReady && p.playbackState == Player.STATE_READY) p.playbackParameters.speed else 0f
        // readyToPlay = the timeline is established, NOT exoplayer's buffering state: for vod that means the duration is known; for live there is no duration so being playable is enough. the sender holds its timeline (and /play) until this, so it must not go true early
        val ready = if (p.isCurrentMediaItemLive) p.playbackState == Player.STATE_READY
                    else durationMs != C.TIME_UNSET
        val snapshot = PlaybackSnapshot(
            position = position,
            duration = duration,
            rate = rate,
            ready = ready,
            playWhenReady = p.playWhenReady,
            speed = p.playbackParameters.speed,
            skipSilence = p.skipSilenceEnabled,
            buffering = p.playbackState == Player.STATE_BUFFERING,
        )
        _snapshot = snapshot
        _speed = snapshot.speed
        // last-resort fill for codecs: the track snapshot can arrive before a selected format is
        // pickable, but by the time playback runs player.videoFormat is populated
        if (_videoFormatInfo.codecs.isEmpty() && _videoFormatInfo.width > 0) {
            val f = p.videoFormat
            if (f != null && f.height > 0) {
                _videoFormatInfo = _videoFormatInfo.copy(
                    codecs = f.codecs ?: f.sampleMimeType ?: "",
                    bitrateBps = f.averageBitrate.toLong(),
                    declaredFps = if (f.frameRate.isFinite() && f.frameRate > 0f) f.frameRate else 0f,
                )
                onVideoFormat?.invoke(_videoFormatInfo)
            }
        }
        // publish the measured render rate once it settles on a new whole number (the container
        // often carries no frame rate at all, so this is the only source for the info line)
        val measured = _measuredFps.roundToInt().toFloat()
        if (_videoFormatInfo.measuredFps != measured) {
            _videoFormatInfo = _videoFormatInfo.copy(measuredFps = measured)
            onVideoFormat?.invoke(_videoFormatInfo)
        }
        onPlaybackInfo?.invoke(snapshot)
    }

    // ---- thread-safe read access (used by the DLNA renderer) ----
    fun currentPositionMs(): Long = (_snapshot.position * 1000).toLong()
    fun durationMs(): Long = (_snapshot.duration * 1000).toLong()
    fun isPlaying(): Boolean = _snapshot.playWhenReady
    fun currentVideoFormat(): VideoFormatInfo = _videoFormatInfo

    companion object {
        private const val TAG = "AirPlayVideoPlayer"
        private const val REPORT_INTERVAL_MS = 250L
        // how far behind the live edge a datagram/RTMP stream is allowed to sit
        private const val LIVE_TARGET_OFFSET_MS = 2000L
        private const val USER_AGENT =
            "Mozilla/5.0 (Linux; Android 10; Android TV) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Mobile Safari/537.36"
        // HLS URLs as sent by Chinese video apps (Youku/QiYi/...), which ExoPlayer's content
        // sniffing may fail to detect -> must be force-loaded on the HLS path
        private fun isHlsUri(url: String): Boolean =
            url.contains(".m3u8", ignoreCase = true) || url.contains("/playlist/m3u8", ignoreCase = true)
        private fun refererHeaders(url: String): Map<String, String> {
            val uri = runCatching { Uri.parse(url) }.getOrNull() ?: return emptyMap()
            val host = uri.host?.lowercase() ?: return emptyMap()
            // many CDNs reject segments unless Referer points at the source site, not the CDN host
            val referer = when {
                host.endsWith(".bilivideo.com") || host.contains("bilibili") -> "https://www.bilibili.com"
                host.contains("youku") || host.endsWith(".tudou.com") || host.contains("tudou") ->
                    "https://www.youku.com"
                else -> (uri.scheme ?: "http") + "://" + host
            }
            return mapOf("Referer" to referer)
        }
    }
}
