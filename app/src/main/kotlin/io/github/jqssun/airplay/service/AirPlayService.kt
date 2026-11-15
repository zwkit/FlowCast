package io.github.jqssun.airplay.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.SharedPreferences
import android.content.pm.ServiceInfo
import android.content.res.Configuration
import android.net.ConnectivityManager
import android.graphics.BitmapFactory
import android.media.AudioManager
import android.os.Binder
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
import android.support.v4.media.MediaMetadataCompat
import android.support.v4.media.session.MediaSessionCompat
import android.support.v4.media.session.PlaybackStateCompat
import android.util.Log
import android.view.Surface
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import androidx.lifecycle.LifecycleService
import androidx.lifecycle.lifecycleScope
import androidx.media.app.NotificationCompat as MediaNotificationCompat
import io.github.jqssun.airplay.MainActivity
import io.github.jqssun.airplay.Prefs
import io.github.jqssun.airplay.R
import io.github.jqssun.airplay.changes
import io.github.jqssun.airplay.get
import io.github.jqssun.airplay.hasLocalNetwork
import io.github.jqssun.airplay.realDisplaySize
import io.github.jqssun.airplay.audio.DacpController
import io.github.jqssun.airplay.audio.DacpPlayer
import io.github.jqssun.airplay.audio.DmapParser
import io.github.jqssun.airplay.audio.TrackInfo
import io.github.jqssun.airplay.audio.VolumeBroadcast
import io.github.jqssun.airplay.bridge.LogListener
import io.github.jqssun.airplay.bridge.NativeBridge
import io.github.jqssun.airplay.bridge.RaopCallbackHandler
import io.github.jqssun.airplay.discovery.MdnsService
import io.github.jqssun.airplay.discovery.NsdServiceManager
import io.github.jqssun.airplay.discovery.MdnsAdvertiser
import io.github.jqssun.airplay.download.VideoDownloader
import io.github.jqssun.airplay.renderer.AirPlayVideoPlayer
import io.github.jqssun.airplay.renderer.AudioConfig
import io.github.jqssun.airplay.renderer.AudioRenderer
import io.github.jqssun.airplay.renderer.VideoFormatInfo
import io.github.jqssun.airplay.renderer.VideoRenderer
import io.github.jqssun.airplay.upnp.DlnaPlayback
import io.github.jqssun.airplay.upnp.DlnaRenderer
import io.github.jqssun.airplay.viewmodel.DebugInfo
import io.github.jqssun.airplay.webpush.WebPushServer
import java.net.Inet4Address
import java.net.NetworkInterface
import java.nio.ByteBuffer
import java.security.SecureRandom
import java.util.UUID
import kotlin.math.abs
import kotlin.math.roundToInt
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.debounce
import kotlinx.coroutines.flow.filter
import kotlinx.coroutines.launch

data class VideoPlaybackInfo(
    val positionMs: Long = 0,
    val durationMs: Long = 0,
    val playing: Boolean = true,
    val speed: Float = 1f,
    val skipSilence: Boolean = false,
    val buffering: Boolean = false,
)

/**
 * One live mirror session as the UI sees it. [aspect] is the source frame aspect the
 * pane must keep, [isPhone] decides which layout rule applies (iPhone* vs iPad/Mac).
 */
data class MirrorPane(
    val id: Long,
    val aspect: Float,
    val resolution: String,
    val deviceName: String,
    val isPhone: Boolean,
    val audioOwner: Boolean,
) {
    val portrait: Boolean get() = aspect < 1f
}

class AirPlayService : LifecycleService(), RaopCallbackHandler, LogListener {

    // read/written from the main thread, native callback threads and the ExoPlayer
    // listener thread; @Volatile keeps cross-thread visibility
    @Volatile private var nativeHandle = 0L
    private var nsdManager: NsdServiceManager? = null
    private var usbAdvertiser: MdnsAdvertiser? = null
    private var lanAdvertiser: MdnsAdvertiser? = null
    private var wakeLock: PowerManager.WakeLock? = null
    private var foregroundStarted = false
    private var lastOrientation = Configuration.ORIENTATION_UNDEFINED

    val videoRenderer = VideoRenderer(this)
    val audioRenderer = AudioRenderer()

    /**
     * 2.0: one renderer per AirPlay mirror session. [videoRenderer] stays the primary
     * instance (idle preview, config template, single-session paths); every mirror
     * session gets its own decoder + GL pipeline so two panes can run at once.
     */
    private val sessions = LinkedHashMap<Long, MirrorSession>()
    private val sessionLock = Any()
    @Volatile private var audioOwnerId = 0L

    private inner class MirrorSession(val id: Long) {
        val renderer = VideoRenderer(this@AirPlayService)
        var aspect = 16f / 9f
        var resolution = ""
        var deviceName = ""
        var model = ""
        var audioOwner = false
        var audioClaimed = false
        var hasVideo = false
        val isPhone: Boolean get() = model.startsWith("iPhone", ignoreCase = true)
    }

    private val _mirrorPanes = MutableStateFlow<List<MirrorPane>>(emptyList())
    /** What the UI renders: one entry per live mirror session, oldest first. */
    val mirrorPanes = _mirrorPanes.asStateFlow()

    private fun _sessionFor(id: Long, create: Boolean): MirrorSession? {
        synchronized(sessionLock) { sessions[id] }?.let { return it }
        if (!create) return null
        val s = MirrorSession(id)
        _configureRenderer(s.renderer)
        s.renderer.onVideoDiscontinuity = {
            if (audioOwnerId == id) audioRenderer.flush()
        }
        s.renderer.audioAnchorProvider = {
            if (audioOwnerId == id) audioRenderer.playoutAnchor() else null
        }
        synchronized(sessionLock) { sessions[id] = s }
        log("Mirror session $id opened (${sessions.size} active)")
        return s
    }

    /** Applies every video preference to a renderer (called on the primary + each session).
     *  Returns what [VideoRenderer.selectDecoders] resolved to, i.e. whether H.265 is usable. */
    private fun _configureRenderer(r: VideoRenderer): Boolean {
        val maxFps = prefs[Prefs.MAX_FPS]
        val (reqW, reqH) = _displaySize(clamp = false)
        val h265 = r.selectDecoders(reqW, reqH, maxFps, prefs[Prefs.H265_ENABLED])
        r.enforceSdr = prefs[Prefs.ENFORCE_SDR]
        r.keyAllowFrameDrop = prefs[Prefs.KEY_ALLOW_FRAME_DROP]
        r.selector.maxOperatingRate = when (prefs[Prefs.OPERATING_RATE]) {
            Prefs.ON -> true; Prefs.OFF -> false; else -> null
        }
        r.benchmarkLog = prefs[Prefs.BENCHMARK_LOG]
        r.benchmarkLogCallback = { msg -> logCallback?.invoke(msg) }
        r.scheduledOutputBufferRelease = prefs[Prefs.SCHEDULED_OUTPUT_BUFFER_RELEASE]
        r.avSyncOffsetNs = prefs[Prefs.AV_SYNC_OFFSET_MS] * 1_000_000L
        return h265
    }

    private fun _publishPanes() {
        val (next, mirroring) = synchronized(sessionLock) {
            // only sessions that actually produced a frame get a pane: a connection that
            // has only done pairing/audio must not light up an empty black half
            val list = sessions.values.filter { it.hasVideo }.map {
                MirrorPane(
                    id = it.id,
                    aspect = it.aspect,
                    resolution = it.resolution,
                    deviceName = it.deviceName,
                    isPhone = it.isPhone,
                    audioOwner = it.audioOwner,
                )
            }
            list to sessions.values.any { it.hasVideo }
        }
        _mirrorPanes.value = next
        _mirroringActive.value = mirroring
    }

    private fun _dropSession(id: Long) {
        val s = synchronized(sessionLock) { sessions.remove(id) } ?: return
        log("Mirror session $id closed (${sessions.size} active)")
        runCatching { s.renderer.stopSession() }
        runCatching { s.renderer.clearDisplay() }
        runCatching { s.renderer.release() }
        if (audioOwnerId == id) {
            // hand the soundtrack to whoever is left instead of going silent
            val next = synchronized(sessionLock) { sessions.keys.lastOrNull() } ?: 0L
            _claimAudio(next, force = true)
        }
        if (synchronized(sessionLock) { sessions.isEmpty() }) {
            _mirroringActive.value = false
            _videoSyncLowLatency(false)
            audioRenderer.stop()
        }
        _publishPanes()
    }

    /**
     * The renderer that owns the screen right now: the audio owner's pane while mirroring,
     * otherwise the newest session with video, otherwise the primary instance (idle preview).
     */
    private fun _activeRenderer(): VideoRenderer = synchronized(sessionLock) {
        sessions[audioOwnerId]?.renderer
            ?: sessions.values.lastOrNull { it.hasVideo }?.renderer
            ?: sessions.values.lastOrNull()?.renderer
            ?: videoRenderer
    }

    private fun _dropAllSessions() {
        val ids = synchronized(sessionLock) { sessions.keys.toList() }
        ids.forEach { _dropSession(it) }
        synchronized(sessionLock) { sessions.clear() }
        _mirrorPanes.value = emptyList()
        audioOwnerId = 0L
        if (nativeHandle != 0L) runCatching { NativeBridge.nativeSetAudioOwner(nativeHandle, 0L) }
    }

    /**
     * Audio arbitration: the most recently opened session owns the soundtrack and the
     * others are muted in native code. Two soundtracks at once is never what a viewer
     * wants (they cover each other), and a single output stream cannot carry both.
     */
    private fun _claimAudio(id: Long, force: Boolean = false) {
        if (!force && audioOwnerId == id) return
        val previous = audioOwnerId
        audioOwnerId = id
        // never swallow a JNI failure here: a missing symbol would leave native arbitration
        // at 0 (both senders audible) and look exactly like "the wrong phone has the sound"
        if (nativeHandle != 0L) {
            runCatching { NativeBridge.nativeSetAudioOwner(nativeHandle, id) }
                .onFailure { log("setAudioOwner($id) failed: $it") }
        }
        synchronized(sessionLock) { sessions.values.forEach { it.audioOwner = it.id == id } }
        if (previous != 0L && previous != id) {
            // a different sender drives the timeline now: restart the audio buffer so the
            // new owner primes its own clock instead of inheriting the old jitter queue
            runCatching { audioRenderer.flush() }
        }
        log("Audio owner -> session $id (was $previous)")
        _publishPanes()
    }

    /**
     * A session joins the audio vote the first time it actually delivers media, and only
     * then. iOS re-announces the audio stream (ANNOUNCE/SETUP -> audio_get_format) on every
     * pause, resume and rotation; claiming on each announce meant the first phone grabbed
     * the soundtrack back within seconds and the later one was never heard.
     */
    private fun _claimAudioFor(s: MirrorSession) {
        if (s.audioClaimed) return
        s.audioClaimed = true
        _claimAudio(s.id)
    }

    private val audioManager by lazy { getSystemService(Context.AUDIO_SERVICE) as AudioManager }
    val airPlayVideoPlayer by lazy { AirPlayVideoPlayer(this) }
    val videoDownloader by lazy { VideoDownloader(this) }

    init {
        // the video stream's pts jumping is the one reliable signal that the
        // mirrored content switched: the audio stream keeps a real-time clock
        // across switches, so its own discontinuity detection usually never
        // fires and the jitter buffer would play the old content's tail first
        videoRenderer.onVideoDiscontinuity = { audioRenderer.flush() }
        // audio is the master clock: the renderer aligns its NTP-scheduled frames
        // to the audio playout anchor, so both streams render the same instant at
        // the same wall-clock instant instead of each anchoring to its own start
        videoRenderer.audioAnchorProvider = { audioRenderer.playoutAnchor() }
    }

    // DLNA (UPnP) receive path: independent ExoPlayer instance, mirrors the display surface
    private val dlnaHttpPort = 8060
    val dlnaVideoPlayer by lazy { AirPlayVideoPlayer(this) }

    // 2.1: LAN page that accepts a video URL from a phone. Own port and own server instance, so
    // the DLNA description does not have to know about it and either one can fail independently.
    private var webPushServer: WebPushServer? = null
    private val _webPushUrl = MutableStateFlow<String?>(null)
    val webPushUrl: StateFlow<String?> = _webPushUrl.asStateFlow()
    // last URL handed over by DLNA or the push page; only used to label the remote control UI
    @Volatile private var externalVideoUrl: String = ""

    // Which player currently owns the screen. DLNA and AirPlay-URL playback share one surface but
    // run on separate ExoPlayers, so every transport call (remote OK / on-screen bar / seek) has
    // to reach the one that is actually playing — the DLNA player used to be driven by nothing,
    // which made the OK key a no-op during a DLNA cast.
    @Volatile private var _dlnaOwnsVideo = false
    private val videoTransport: AirPlayVideoPlayer
        get() = if (_dlnaOwnsVideo) dlnaVideoPlayer else airPlayVideoPlayer

    private val dlnaRenderer by lazy {
        DlnaRenderer(
            this,
            dlnaHttpPort,
            // per-install UPnP UDN, see Prefs.DLNA_UDN
            prefs.getString(Prefs.DLNA_UDN, null)
                ?: UUID.randomUUID().toString().also {
                    prefs.edit().putString(Prefs.DLNA_UDN, it).apply()
                },
            { prefs[Prefs.SERVER_NAME].ifBlank { Prefs.defaultDeviceName } },
            object : DlnaPlayback {
                override fun play(uri: String, title: String?) =
                    _playExternalUrl(uri, title, "DLNA cast")
                override fun pause() { dlnaVideoPlayer.setPlaying(false) }
                override fun resume() { dlnaVideoPlayer.setPlaying(true) }
                override fun stop() {
                    dlnaVideoPlayer.stop()
                    if (_videoPlaybackActive.value) _endVideoPlayback("DLNA stopped")
                }
                override fun seek(seconds: Long) { dlnaVideoPlayer.scrub(seconds.toFloat()) }
                override fun positionSeconds(): Long = dlnaVideoPlayer.currentPositionMs() / 1000
                override fun durationSeconds(): Long = dlnaVideoPlayer.durationMs() / 1000
                override fun isPlaying(): Boolean = dlnaVideoPlayer.isPlaying()
            },
        )
    }

    /**
     * Hands an external URL to the video player. Shared by the DLNA renderer and the web-push
     * page: both must stop whatever was playing, take the display over, evict a live AirPlay
     * session and bring the activity forward, or the new cast competes with the old one.
     */
    private fun _playExternalUrl(uri: String, title: String?, source: String) {
        // free the shared display surface from any AirPlay URL playback
        airPlayVideoPlayer.stop()
        dlnaVideoPlayer.stop()
        _disconnecting = false
        _videoPollSuppressed = false
        // claim the surface BEFORE evicting the AirPlay session: that eviction clears the
        // mirroring flag, and a moment with both flags false reads as "cast ended" and sends
        // the app to the background
        _videoPlaybackActive.value = true
        // the new cast takes the receiver over: a live AirPlay mirror (or a music stream) must
        // go, or its audio keeps playing on top of the new cast — two soundtracks at once
        _evictAirPlaySession(source)
        _videoPlaySeq.value += 1L
        _videoTitle.value = title ?: ""
        externalVideoUrl = uri
        _dlnaOwnsVideo = true
        dlnaVideoPlayer.play(uri, 0f)
        launchMainActivity()
    }

    // 2.1 web push: a LAN page behind a QR code that hands a video URL to the receiver

    private fun _startWebPush() {
        _stopWebPush()
        if (!prefs[Prefs.WEB_PUSH]) return
        val token = _newPushToken()
        val server = WebPushServer(
            preferredPort = prefs[Prefs.WEB_PUSH_PORT],
            token = token,
            handler = object : WebPushServer.Handler {
                override fun play(url: String) {
                    log("Web push: play requested")
                    _playExternalUrl(url, null, "web push")
                }

                override fun control(action: String, positionSeconds: Float) {
                    if (!_videoPlaybackActive.value) return
                    when (action) {
                        "pause" -> videoTransport.setPlaying(false)
                        "resume" -> videoTransport.setPlaying(true)
                        "seek" -> if (positionSeconds >= 0f) videoTransport.scrub(positionSeconds)
                        "stop" -> {
                            videoTransport.stop()
                            _endVideoPlayback("web push stopped")
                        }
                    }
                }

                override fun snapshot(): WebPushServer.Snapshot {
                    val active = _videoPlaybackActive.value
                    return WebPushServer.Snapshot(
                        active = active,
                        url = externalVideoUrl,
                        title = _videoTitle.value,
                        playing = active && videoTransport.isPlaying(),
                        positionSeconds = videoTransport.currentPositionMs() / 1000f,
                        durationSeconds = videoTransport.durationMs() / 1000f,
                    )
                }
            },
        )
        val ip = _localIpv4()
        if (ip == null) {
            log("Web push: no LAN address, page disabled")
            return
        }
        if (!server.start()) {
            log("Web push: no free port near ${prefs[Prefs.WEB_PUSH_PORT]}")
            return
        }
        webPushServer = server
        // the full URL, token included, goes to the run log on purpose: it is the only way to
        // recover the page when a phone camera refuses to scan, and it dies with the service
        _webPushUrl.value = "http://$ip:${server.port}/?k=$token"
        log("Web push: http://$ip:${server.port}/?k=$token")
    }

    private fun _stopWebPush() {
        webPushServer?.stop()
        webPushServer = null
        _webPushUrl.value = null
    }

    // no 0/O/1/l: the token stays readable if someone types it instead of scanning
    private fun _newPushToken(): String {
        val alphabet = "abcdefghjkmnpqrstuvwxyz23456789"
        val rnd = SecureRandom()
        return (0 until 12).map { alphabet[rnd.nextInt(alphabet.length)] }.joinToString("")
    }

    /** first usable IPv4 of the interface that actually carries traffic */
    private fun _localIpv4(): String? = runCatching {
        val cm = getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        val active = cm.activeNetwork?.let { cm.getLinkProperties(it)?.interfaceName }
        var fallback: String? = null
        for (ni in NetworkInterface.getNetworkInterfaces()?.toList().orEmpty()) {
            if (!ni.isUp || ni.isLoopback) continue
            val name = ni.name.lowercase()
            if (name.startsWith("p2p") || name.startsWith("dummy") || name.startsWith("rmnet") ||
                name.startsWith("tun") || name.startsWith("wfd")
            ) continue
            for (addr in ni.inetAddresses.toList()) {
                if (addr !is Inet4Address) continue
                val ip = addr.hostAddress ?: continue
                if (ip.startsWith("169.254")) continue
                if (ni.name == active) return@runCatching ip
                if (fallback == null) fallback = ip
            }
        }
        fallback
    }.getOrNull()

    // hls urls point at the native httpd, valid only while the session lives
    private val _videoLocation = MutableStateFlow<String?>(null)
    val videoLocation = _videoLocation.asStateFlow()

    private val prefs: SharedPreferences by lazy {
        getSharedPreferences(Prefs.NAME, Context.MODE_PRIVATE)
    }
    private val _serverState = MutableStateFlow(ServerState.STOPPED)
    val serverState = _serverState.asStateFlow()

    private val _connectionCount = MutableStateFlow(0)
    val connectionCount = _connectionCount.asStateFlow()

    private val _videoAspect = MutableStateFlow(16f / 9f)
    val videoAspect = _videoAspect.asStateFlow()

    private val _videoResolution = MutableStateFlow("")
    val videoResolution = _videoResolution.asStateFlow()

    private val _audioOnly = MutableStateFlow(false)
    val audioOnly = _audioOnly.asStateFlow()

    private val _videoPlaybackActive = MutableStateFlow(false)
    val videoPlaybackActive = _videoPlaybackActive.asStateFlow()

    // base native audio buffer config (adaptive tuner by default). AirPlay video plays the
    // picture via ExoPlayer but its audio arrives as a separate native RAOP stream; the
    // adaptive cushion can grow to ~1s, which makes audio lag far behind the near-instant
    // video. We pin a small fixed cushion while a video session is active, then restore this.
    private var _baseAudioConfig: AudioConfig = AudioConfig()
    private val _videoSyncCushionMs = 80
    private fun _videoSyncLowLatency(enabled: Boolean) {
        // mirroring: small fixed cushion + oboe low-latency output (Game usage, two-burst
        // buffer) + low-latency decoder. the adaptive tuner is for music, where a large
        // cushion is fine; for mirroring it just lags audio behind the near-instant video
        audioRenderer.updateConfig(
            if (enabled) _baseAudioConfig.copy(cushionMs = _videoSyncCushionMs, lowLatency = true)
            else _baseAudioConfig
        )
    }

    private val _videoPlaybackInfo = MutableStateFlow(VideoPlaybackInfo())
    val videoPlaybackInfo = _videoPlaybackInfo.asStateFlow()

    // bumped per /play so the ui resets transport state on back-to-back plays too
    private val _videoPlaySeq = MutableStateFlow(0L)
    val videoPlaySeq = _videoPlaySeq.asStateFlow()

    private val _videoPlaybackAspect = MutableStateFlow(16f / 9f)
    val videoPlaybackAspect = _videoPlaybackAspect.asStateFlow()

    private val _videoPlaybackSize = MutableStateFlow<Pair<Int, Int>?>(null)
    val videoPlaybackSize = _videoPlaybackSize.asStateFlow()

    // codec / resolution / frame rate of the track being played, shown in the player overlay
    private val _videoFormat = MutableStateFlow(VideoFormatInfo())
    val videoFormat = _videoFormat.asStateFlow()

    // container/manifest metadata
    private val _videoTitle = MutableStateFlow("")
    val videoTitle = _videoTitle.asStateFlow()

    // recent /playback-info polls with no playback = pending video; polls start ~1s before /play
    @Volatile private var _lastVideoPollAt = 0L
    @Volatile private var _videoPollSuppressed = false
    // set while a back-key disconnect is tearing down: suppresses the native poll / disconnect
    // callbacks that would otherwise re-flag a pending video session and leave the UI stuck on a
    // black fullscreen right after leaving the cast. Cleared on the next real connection/play.
    @Volatile private var _disconnecting = false

    fun videoSessionPending(): Boolean =
        !_disconnecting && !_videoPlaybackActive.value && !_audioOnly.value && !_mirroringActive.value &&
            !_videoPollSuppressed &&
            SystemClock.elapsedRealtime() - _lastVideoPollAt < VIDEO_POLL_PENDING_TIMEOUT_MS

    // set once mirroring reports a real size; stops with session
    private val _mirroringActive = MutableStateFlow(false)
    val mirroringActive = _mirroringActive.asStateFlow()

    private val _trackInfo = MutableStateFlow(TrackInfo())
    val trackInfo = _trackInfo.asStateFlow()

    private val _positionMs = MutableStateFlow(0L)
    val positionMs = _positionMs.asStateFlow()

    private val _durationMs = MutableStateFlow(0L)
    val durationMs = _durationMs.asStateFlow()

    private val _playing = MutableStateFlow(true)
    val playing = _playing.asStateFlow()

    // (positionMs, elapsedRealtimeAt) published as one reference so readers never see a
    // torn pair (new position with old timestamp or vice versa)
    private class ProgressAnchor(val positionMs: Long, val atMs: Long)
    @Volatile private var _progressAnchor: ProgressAnchor? = null

    fun currentPositionMs(): Long {
        val anchor = _progressAnchor ?: return _positionMs.value
        if (!_playing.value) return _positionMs.value
        val elapsed = SystemClock.elapsedRealtime() - anchor.atMs
        return (anchor.positionMs + elapsed).coerceIn(0, _durationMs.value)
    }

    var dacpController: DacpController? = null
        private set
    lateinit var dacpPlayer: DacpPlayer
        private set
    private val _mainHandler = Handler(Looper.getMainLooper())
    private var _disconnectRestart: Runnable? = null
    // how long the servers stay physically stopped after a back-key disconnect before the
    // native + DLNA listeners come back up; long enough for the still-connected client to tear
    // down (so it does NOT auto-reconnect and loop), short enough that the device stays castable
    // again without a manual restart
    private val DISCONNECT_RESTART_DELAY_MS = 5000L
    // shorter: here the new cast is already playing, the AirPlay server just has to be
    // reachable again quickly for the next sender
    private val EVICT_RESTART_DELAY_MS = 1500L
    @Volatile private var _coverArtBytes: ByteArray? = null
    private var mediaSession: MediaSessionCompat? = null
    private var mediaReceiver: BroadcastReceiver? = null
    private var volumeReceiver: BroadcastReceiver? = null
    private var _pendingVolEchoes = 0
    // senders park volume at -30 as route closes; session must not leave device silenced
    private var _preZeroIdx = -1
    @Volatile private var _senderFrac = -1f
    private var _volSyncTarget = -1f
    private var _volSyncHint = 0
    private var _volSyncDir = 0
    private var _volSyncSteps = 0
    private val _volSyncTimeout = Runnable { _volSyncEnd() }

    var logCallback: ((String) -> Unit)? = null

    @Volatile private var _lastPin: String? = null
    var pinCallback: ((String?) -> Unit)? = null
        set(value) {
            field = value
            // ui replay only: binding the activity must not mint a new native pin
            value?.invoke(_lastPin)
        }

    private fun log(msg: String) {
        Log.i(TAG, msg)
        logCallback?.invoke(msg)
    }

    override fun onLog(msg: String) {
        logCallback?.invoke(msg)
    }

    inner class LocalBinder : Binder() {
        val service: AirPlayService
            get() = this@AirPlayService
    }

    override fun onBind(intent: Intent): IBinder {
        super.onBind(intent)
        return LocalBinder()
    }

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
        dacpController = DacpController(this)
        dacpPlayer = DacpPlayer(
            mainLooper,
            dacp = { dacpController },
            snapshot = {
                DacpPlayer.Snapshot(
                    track = _trackInfo.value,
                    artworkData = _coverArtBytes,
                    durationMs = _durationMs.value,
                    playing = _playing.value,
                    active = _audioOnly.value && _connectionCount.value > 0,
                )
            },
            positionMs = ::currentPositionMs,
            setPlaying = ::_setPlaying,
        )
        mediaSession = MediaSessionCompat(this, "AirPlay").apply {
            setCallback(object : MediaSessionCompat.Callback() {
                override fun onPlay() {
                    if (_videoPlaybackActive.value) {
                        videoTransport.setPlaying(true)
                        return
                    }
                    _setPlaying(true)
                    dacpController?.play()
                }
                override fun onPause() {
                    if (_videoPlaybackActive.value) {
                        videoTransport.setPlaying(false)
                        return
                    }
                    _setPlaying(false)
                    dacpController?.pause()
                }
                override fun onStop() {
                    if (_videoPlaybackActive.value) stopVideoPlayback()
                }
                override fun onFastForward() {
                    if (_videoPlaybackActive.value) videoTransport.seekBy(VIDEO_SEEK_STEP_MS)
                }
                override fun onRewind() {
                    if (_videoPlaybackActive.value) videoTransport.seekBy(-VIDEO_SEEK_STEP_MS)
                }
                override fun onSeekTo(pos: Long) {
                    if (_videoPlaybackActive.value) videoTransport.scrub(pos / 1000f)
                }
                override fun onSkipToNext() {
                    if (!_videoPlaybackActive.value) dacpController?.nextItem()
                }
                override fun onSkipToPrevious() {
                    if (!_videoPlaybackActive.value) dacpController?.prevItem()
                }
            })
        }
        mediaReceiver = object : BroadcastReceiver() {
            override fun onReceive(ctx: Context, intent: Intent) {
                when (intent.action) {
                    ACTION_PLAY_PAUSE -> {
                        // the notification transport button must drive whichever session is live:
                        // video playback is controlled through ExoPlayer, everything else through
                        // the DACP/music path (togglePlayPause only touches the audio-only session)
                        if (_videoPlaybackActive.value) {
                            airPlayVideoPlayer.setPlaying(!airPlayVideoPlayer.isPlaying())
                        } else {
                            togglePlayPause()
                        }
                    }
                    ACTION_NEXT -> if (!_videoPlaybackActive.value) dacpController?.nextItem()
                    ACTION_PREV -> if (!_videoPlaybackActive.value) dacpController?.prevItem()
                }
            }
        }
        val filter = IntentFilter().apply {
            addAction(ACTION_PLAY_PAUSE)
            addAction(ACTION_NEXT)
            addAction(ACTION_PREV)
        }
        ContextCompat.registerReceiver(this, mediaReceiver, filter, ContextCompat.RECEIVER_NOT_EXPORTED)

        volumeReceiver = object : BroadcastReceiver() {
            override fun onReceive(ctx: Context, intent: Intent) {
                if (intent.getIntExtra(VolumeBroadcast.EXTRA_STREAM_TYPE, -1) != AudioManager.STREAM_MUSIC) return
                val idx = intent.getIntExtra(VolumeBroadcast.EXTRA_VALUE, -1)
                val prev = intent.getIntExtra(VolumeBroadcast.EXTRA_PREV_VALUE, -1)
                if (idx < 0 || prev < 0 || idx == prev) return
                if (_pendingVolEchoes > 0) { _pendingVolEchoes--; return }
                if (_connectionCount.value == 0) return
                val target = idx.toFloat() / audioManager.getStreamMaxVolume(AudioManager.STREAM_MUSIC)
                Log.d(TAG, "local volume $prev -> $idx, sync sender to $target")
                _preZeroIdx = -1
                val active = _volSyncTarget >= 0f
                _volSyncTarget = target
                _volSyncHint = if (idx > prev) 1 else -1
                _volSyncDir = 0
                _volSyncSteps = 0
                if (!active) _volSyncStep()
            }
        }
        ContextCompat.registerReceiver(this, volumeReceiver, IntentFilter(VolumeBroadcast.ACTION),
            ContextCompat.RECEIVER_NOT_EXPORTED)

        airPlayVideoPlayer.onVideoSize = { width, height, aspect ->
            _videoPlaybackAspect.value = aspect
            _videoPlaybackSize.value = width to height
        }
        airPlayVideoPlayer.onVideoFormat = { _videoFormat.value = it }
        airPlayVideoPlayer.onTitle = { _videoTitle.value = it ?: "" }
        airPlayVideoPlayer.onEnded = { _endVideoPlayback("AirPlay Video stopped (player)") }
        // DLNA (UPnP) playback drives the same on-screen video surface / media session
        dlnaVideoPlayer.onVideoSize = { width, height, aspect ->
            _videoPlaybackAspect.value = aspect
            _videoPlaybackSize.value = width to height
        }
        dlnaVideoPlayer.onVideoFormat = { _videoFormat.value = it }
        dlnaVideoPlayer.onTitle = { _videoTitle.value = it ?: "" }
        dlnaVideoPlayer.onPlaybackInfo = { snapshot ->
            // a disconnect suppresses these polls so a stale playback callback can't bounce the
            // UI back onto the black video fullscreen after back-key teardown
            if (!_videoPollSuppressed) {
                // only a real playback snapshot (nonzero duration/position, or actually
                // playing) should mark video playback active. A stop()/teardown callback comes
                // back with all fields zeroed; unconditionally setting the flag here would flip
                // _videoPlaybackActive back to true and leave the overview on a black fullscreen
                // after a server restart.
                val hasContent = snapshot.duration > 0f || snapshot.position > 0f ||
                    snapshot.playWhenReady || snapshot.rate != 0f
                if (hasContent) _videoPlaybackActive.value = true
                _updateVideoPlaybackState(snapshot.position, snapshot.rate)
                _videoPlaybackInfo.value = VideoPlaybackInfo(
                    positionMs = (snapshot.position * 1000).toLong(),
                    durationMs = if (snapshot.duration > 0f) (snapshot.duration * 1000).toLong() else 0L,
                    playing = snapshot.playWhenReady,
                    speed = snapshot.speed,
                    skipSilence = snapshot.skipSilence,
                    buffering = snapshot.buffering,
                )
            }
        }
        dlnaVideoPlayer.onEnded = { _endVideoPlayback("DLNA playback ended") }
        airPlayVideoPlayer.onPlaybackInfo = { snapshot ->
            if (nativeHandle != 0L) {
                NativeBridge.nativeUpdatePlaybackInfo(nativeHandle, snapshot.position, snapshot.duration, snapshot.rate, snapshot.ready)
            }
            if (_videoPlaybackActive.value) {
                _updateVideoPlaybackState(snapshot.position, snapshot.rate)
                _videoPlaybackInfo.value = VideoPlaybackInfo(
                    positionMs = (snapshot.position * 1000).toLong(),
                    durationMs = if (snapshot.duration > 0f) (snapshot.duration * 1000).toLong() else 0L,
                    playing = snapshot.playWhenReady,
                    speed = snapshot.speed,
                    skipSilence = snapshot.skipSilence,
                    buffering = snapshot.buffering,
                )
            }
        }
        lifecycleScope.launch {
            prefs.audioConfigFlow()
                .debounce(AUDIO_CONFIG_DEBOUNCE_MS)
                .collect {
                    _baseAudioConfig = it
                    audioRenderer.updateConfig(it)
                }
        }
        lifecycleScope.launch {
            prefs.changes()
                .filter { Prefs.all[it]?.restart == true }
                .debounce(SERVER_RESTART_DEBOUNCE_MS)
                .collect { _restartServer() }
        }
        lifecycleScope.launch {
            // A/V sync nudge takes effect mid-session: the renderer reads the
            // offset per frame, so dragging the slider re-times the next frame
            prefs.changes()
                .filter { it == Prefs.AV_SYNC_OFFSET_MS.key }
                .collect {
                    val offsetNs = prefs[Prefs.AV_SYNC_OFFSET_MS] * 1_000_000L
                    videoRenderer.avSyncOffsetNs = offsetNs
                    // 2.0: the knob has to reach every live pane, not just the primary renderer
                    synchronized(sessionLock) {
                        sessions.values.forEach { it.renderer.avSyncOffsetNs = offsetNs }
                    }
                }
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_START_SERVER) {
            promoteToForeground()
            _startServer(ensureServiceStarted = false)
            if (_serverState.value != ServerState.RUNNING) stopSelf(startId)
        }
        return START_NOT_STICKY
    }

    fun startServer() {
        _startServer(ensureServiceStarted = true)
    }

    private fun _startServer(ensureServiceStarted: Boolean) {
        if (_serverState.value == ServerState.RUNNING) return
        if (!hasLocalNetwork()) {
            log("Local network permission denied")
            _failStart()
            return
        }
        val effectiveName = prefs[Prefs.SERVER_NAME].ifBlank { Prefs.defaultDeviceName }

        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "airplay:server").apply { acquire() }

        nsdManager = NsdServiceManager(this).apply { acquireMulticastLock() }

        val hwAddr = getHwAddr()
        val keyFile = filesDir.resolve("airplay.pem").absolutePath
        val nohold = prefs[Prefs.ALLOW_NEW_CONN]
        val requirePin = prefs[Prefs.REQUIRE_PIN]
        // AirPlay pairing identity: unique per install. a shared one (UxPlay's hardcoded
        // UUID) makes two receivers on the LAN look like the same accessory to iOS, which
        // then connects to the wrong one whenever both are up
        val pairingId = prefs.getString(Prefs.AIRPLAY_PAIRING_ID, null)
            ?: UUID.randomUUID().toString().also {
                prefs.edit().putString(Prefs.AIRPLAY_PAIRING_ID, it).apply()
            }

        // oboe's OpenSL ES backend (pre-AAudio devices, API < 27) can't discover native
        // rate / burst size itself; feed it AudioManager values so low-latency buffer
        // sizing works there
        // see: https://github.com/google/oboe/blob/main/docs/GettingStarted.md#obtaining-optimal-latency
        NativeBridge.nativeSetDefaultStreamValues(
            audioManager.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE)?.toIntOrNull() ?: 0,
            audioManager.getProperty(AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER)?.toIntOrNull() ?: 0
        )
        nativeHandle = NativeBridge.nativeInit(
            this, hwAddr, effectiveName, keyFile, nohold, requirePin, pairingId
        )
        if (nativeHandle == 0L) {
            log("Native init failed")
            _failStart()
            return
        }
        audioRenderer.attachEngine(nativeHandle)

        // apply settings from preferences
        val maxFps = prefs[Prefs.MAX_FPS]
        val overscanned = prefs[Prefs.OVERSCANNED]
        val audioLatencyMs = prefs[Prefs.AUDIO_LATENCY_MS]
        // same video preferences for the primary renderer and every mirror session
        val h265 = _configureRenderer(videoRenderer)
        val alac = prefs[Prefs.ALAC_ENABLED]
        val aac = prefs[Prefs.AAC_ENABLED]

        // 2.0: up to two senders may mirror at once; 1 keeps the classic exclusive behaviour
        NativeBridge.nativeSetMaxSessions(
            nativeHandle,
            if (prefs[Prefs.MULTI_SESSION]) 2 else 1
        )
        NativeBridge.nativeSetH265Enabled(nativeHandle, h265)
        NativeBridge.nativeSetCodecs(nativeHandle, alac, aac)
        val advertiseVideo = prefs[Prefs.ADVERTISE_VIDEO]
        val advertiseMirroring = prefs[Prefs.ADVERTISE_MIRRORING]
        val advertiseAudio = prefs[Prefs.ADVERTISE_AUDIO]
        NativeBridge.nativeSetHlsEnabled(nativeHandle, advertiseVideo)
        NativeBridge.nativeSetLang(nativeHandle, "", "", resources.configuration.locales.toLanguageTags().replace(',', ':'))
        NativeBridge.nativeSetFeature(nativeHandle, NativeBridge.FT_MIRROR, advertiseMirroring)
        NativeBridge.nativeSetFeature(nativeHandle, NativeBridge.FT_AUDIO, advertiseAudio)
        NativeBridge.nativeSetPlist(nativeHandle, "maxFPS", maxFps)
        NativeBridge.nativeSetPlist(nativeHandle, "overscanned", if (overscanned) 1 else 0)
        if (audioLatencyMs >= 0) {
            NativeBridge.nativeSetPlist(nativeHandle, "audio_delay_micros", audioLatencyMs * 1000)
        }

        // set display params
        lastOrientation = resources.configuration.orientation
        val (w, h) = _displaySize()
        videoRenderer.setResolution(w, h)
        _videoResolution.value = "${w}x${h}"
        _videoAspect.value = w.toFloat() / h
        NativeBridge.nativeSetDisplaySize(nativeHandle, w, h, maxFps)

        val requestedPort = prefs[Prefs.SERVER_PORT]
        val port = NativeBridge.nativeStart(nativeHandle, requestedPort)
        if (port < 0) {
            log("Failed to start on port $requestedPort")
            _failStart()
            return
        }

        // register mdns services
        val raopTxt = NativeBridge.nativeGetRaopTxtRecords(nativeHandle) ?: emptyMap()
        val airplayTxt = NativeBridge.nativeGetAirplayTxtRecords(nativeHandle) ?: emptyMap()
        val resolvedName = NativeBridge.nativeGetServerName(nativeHandle) ?: effectiveName

        val services = buildList {
            // Advertise both _airplay._tcp and _raop._tcp under the SAME instance name
            // (the user-configured server name). Previously _raop used a separate
            // "hwaddr@name" instance, which iOS exposed as a second, audio-only device
            // that kept the old name after a rename. Sharing the name makes iOS merge
            // them into one device and keeps both names in sync with the setting.
            if (advertiseAudio) add(MdnsService(resolvedName, "_raop._tcp", port, raopTxt))
            add(MdnsService(resolvedName, "_airplay._tcp", port, airplayTxt))
        }
        // hostname suffix for our own mDNS responder: stable across restarts (the SRV target
        // iOS caches must not change) and unique per device. Advertising ourselves instead of
        // letting NsdManager publish the shared system hostname ("Android.local", "
        // Android-2.local", ...) is what stops a second receiver on the same LAN from taking
        // over the name — and with it the phone's cached connection
        val hostId = hwAddr.joinToString("") { "%02x".format(it) }.takeLast(6)
        if (prefs[Prefs.USB_ONLY]) {
            usbAdvertiser = MdnsAdvertiser(services, MdnsAdvertiser.USB_IFACE, "airplay-usb", hostId) { iface, addr ->
                log(if (iface == null) "USB link down" else "USB link up on $iface (${addr?.hostAddress})")
            }.apply { start() }
        } else if (prefs[Prefs.MDNS_LEGACY_NSD]) {
            // escape hatch for support: advertise through NsdManager exactly as before
            nsdManager?.register(services)
        } else {
            lanAdvertiser = MdnsAdvertiser(
                services, hostId = hostId, preferredIface = _activeIfaceName(),
                checkConflicts = false
            ) { iface, addr ->
                log(if (iface == null) "mDNS offline" else "mDNS on $iface (${addr?.hostAddress})")
            }.apply { start() }
        }

        _serverState.value = ServerState.RUNNING
        isServerRunning = true
        if (ensureServiceStarted) {
            ContextCompat.startForegroundService(this, Intent(this, AirPlayService::class.java))
        }
        promoteToForeground()
        log("Server started on port $port")
        runCatching { dlnaRenderer.start() }.onFailure { Log.w(TAG, "DLNA start failed", it) }
        runCatching { _startWebPush() }.onFailure { Log.w(TAG, "Web push start failed", it) }
    }

    // interface that currently carries traffic: a box can have both eth0 and wlan0 up with
    // an address, and only one of them reaches the phone
    private fun _activeIfaceName(): String? = try {
        val cm = getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        cm.activeNetwork?.let { cm.getLinkProperties(it)?.interfaceName }
    } catch (_: Exception) {
        null
    }

    private fun _orientationFollowsDevice(): Boolean =
        prefs[Prefs.RESOLUTION] == Prefs.AUTO

    private fun _displaySize(clamp: Boolean = true): Pair<Int, Int> {
        val res = prefs[Prefs.RESOLUTION]
        val portrait = when (res) {
            Prefs.PORTRAIT -> true
            Prefs.LANDSCAPE -> false
            else -> resources.configuration.orientation == Configuration.ORIENTATION_PORTRAIT
        }
        val (rawW, rawH) = realDisplaySize()
        val device = if (portrait != rawH >= rawW) rawH to rawW else rawW to rawH
        if (res == Prefs.PORTRAIT || res == Prefs.LANDSCAPE) return device
        val (w, h) = if (res.contains("x")) {
            res.split("x").let { it[0].toInt() to it[1].toInt() }
        } else device
        if (!clamp) return w to h
        // strict decoders black-screen past their limits; advertised size is only upper bound for senders
        val (maxW, maxH) = videoRenderer.maxResolution()
        return w.coerceAtMost(maxW) to h.coerceAtMost(maxH)
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        if (newConfig.orientation == lastOrientation) return
        lastOrientation = newConfig.orientation
        if (nativeHandle == 0L || _serverState.value != ServerState.RUNNING) return
        if (!_orientationFollowsDevice()) return
        val (w, h) = _displaySize()
        NativeBridge.nativeSetDisplaySize(nativeHandle, w, h, prefs[Prefs.MAX_FPS])
        log("Advertising ${w}x${h} from next session")
    }

    private fun _failStart() {
        audioRenderer.detachEngine()
        if (nativeHandle != 0L) {
            NativeBridge.nativeDestroy(nativeHandle)
            nativeHandle = 0L
        }
        lanAdvertiser?.stop()
        lanAdvertiser = null
        nsdManager?.release()
        nsdManager = null
        wakeLock?.release()
        wakeLock = null
        _serverState.value = ServerState.ERROR
        isServerRunning = false
        if (foregroundStarted) {
            stopForeground(STOP_FOREGROUND_REMOVE)
            foregroundStarted = false
        }
    }

    fun stopServer() {
        _shutdown()
        stopForeground(STOP_FOREGROUND_REMOVE)
        foregroundStarted = false
        stopSelf()
    }

    private fun _restartServer() {
        if (_serverState.value != ServerState.RUNNING) return
        _shutdown()
        _startServer(ensureServiceStarted = false)
        if (_serverState.value != ServerState.RUNNING) stopSelf()
    }

    private fun _shutdown() {
        // 2.0: release every mirror pane's decoder/GL pipeline before the audio engine goes away
        _dropAllSessions()
        _stopWebPush()
        audioRenderer.detachEngine()
        if (nativeHandle != 0L) {
            NativeBridge.nativeStop(nativeHandle)
            NativeBridge.nativeDestroy(nativeHandle)
            nativeHandle = 0L
        }
        dacpController?.reset()
        usbAdvertiser?.stop()
        usbAdvertiser = null
        lanAdvertiser?.stop()
        lanAdvertiser = null
        nsdManager?.release()
        nsdManager = null
        wakeLock?.release()
        wakeLock = null
        // suppress playback/poll callbacks before stopping the players: both ExoPlayer instances
        // fire onPlaybackInfo on stop(), and unless suppressed that callback unconditionally sets
        // _videoPlaybackActive back to true. If that landed after the reset below it would leave
        // the flag true, so after a restart videoScreen stays true and the overview renders a
        // black fullscreen until the activity is recreated. Leave it suppressed -- a fresh play
        // (onVideoPlay / DLNA play) clears it.
        _videoPollSuppressed = true
        // black out the display surface before tearing the pipeline down, so a stale last frame
        // doesn't linger black-on-black / residual on the overview after stop
        videoRenderer.clearDisplay()
        videoRenderer.release()
        airPlayVideoPlayer.stop()
        dlnaRenderer.stop()
        dlnaVideoPlayer.stop()
        mediaSession?.isActive = false
        _audioOnly.value = false
        _videoPlaybackActive.value = false
        _mirroringActive.value = false
        _videoSyncLowLatency(false)
        _videoPlaybackInfo.value = VideoPlaybackInfo()
        _lastVideoPollAt = 0
        // restore polling; the stop-callback suppression above only guards the synchronous stop
        // window, and a real subsequent play (onVideoPlay / DLNA play) re-arms it anyway
        _videoPollSuppressed = false
        _coverArtBytes = null
        _trackInfo.value = TrackInfo()
        _positionMs.value = 0
        _durationMs.value = 0
        _serverState.value = ServerState.STOPPED
        isServerRunning = false
        _connectionCount.value = 0
        _refreshDacpPlayer()
        log("Server stopped")
    }

    fun setVideoSurface(surface: Surface) {
        videoRenderer.setSurface(surface)
    }

    fun clearVideoSurface(surface: Surface) {
        videoRenderer.clearSurface(surface)
    }

    /** 2.0: binds one pane's SurfaceView to its own session renderer. */
    fun setPaneSurface(sessionId: Long, surface: Surface) {
        _sessionFor(sessionId, create = true)?.renderer?.setSurface(surface)
    }

    fun clearPaneSurface(sessionId: Long, surface: Surface) {
        val s = synchronized(sessionLock) { sessions[sessionId] } ?: return
        runCatching { s.renderer.clearSurface(surface) }
        runCatching { s.renderer.clearDisplay() }
    }

    fun setVideoPlaybackSurface(surface: Surface) {
        airPlayVideoPlayer.setSurface(surface)
    }

    fun clearVideoPlaybackSurface(surface: Surface) {
        airPlayVideoPlayer.clearSurface(surface)
    }

    fun setDlnaVideoSurface(surface: Surface) {
        dlnaVideoPlayer.setSurface(surface)
    }

    fun clearDlnaVideoSurface(surface: Surface) {
        dlnaVideoPlayer.clearSurface(surface)
    }

    fun setVideoPlaying(playing: Boolean) {
        videoTransport.setPlaying(playing)
    }

    fun seekVideoTo(positionMs: Long) {
        videoTransport.scrub(positionMs / 1000f)
    }

    fun setVideoScrubbing(enabled: Boolean) {
        videoTransport.setScrubbing(enabled)
    }

    fun setVideoSpeed(speed: Float) {
        videoTransport.setSpeed(speed)
    }

    fun setVideoSkipSilence(enabled: Boolean) {
        videoTransport.setSkipSilence(enabled)
    }

    fun seekVideoBy(deltaMs: Long) {
        videoTransport.seekBy(deltaMs)
    }

    fun stopVideoPlayback() = _endVideoPlayback("AirPlay Video stopped (local)")

    /** Tear down whatever is being cast (mirroring / video / DLNA / audio-only) while the
     *  service stays alive in the background for the next cast. Used on back-key exit. */
    fun disconnectCast() {
        _disconnecting = true
        _videoPollSuppressed = true
        _lastVideoPollAt = 0
        // force-tear every cast path regardless of current state
        _videoPlaybackActive.value = false
        _mirroringActive.value = false
        _videoSyncLowLatency(false)
        _audioOnly.value = false
        _videoPlaybackInfo.value = VideoPlaybackInfo()
        airPlayVideoPlayer.stop()
        dlnaVideoPlayer.stop()
        // 2.0: tear down every mirror pane's renderer, not just the primary one
        _dropAllSessions()
        videoRenderer.stopSession()
        // wipe the display surface so the stale last frame (a residual phone-screen grab on
        // TV) doesn't linger on the overview preview after the mirror/video session ends
        videoRenderer.clearDisplay()
        audioRenderer.stop()
        mediaSession?.isActive = false
        // Physically stop both servers now so the still-connected casting client tears down and
        // does NOT auto-reconnect (restarting instantly made it reconnect and loop, leaving the
        // back key unable to quit and a black fullscreen behind). The servers then come back up
        // after a short delay so the device stays castable again without a manual restart.
        dlnaRenderer.stop()
        if (nativeHandle != 0L) runCatching { NativeBridge.nativeStop(nativeHandle) }
        _serverState.value = ServerState.RUNNING
        _disconnectRestart?.let { _mainHandler.removeCallbacks(it) }
        val restart = Runnable {
            _disconnectRestart = null
            if (nativeHandle != 0L) {
                runCatching { NativeBridge.nativeStart(nativeHandle, prefs[Prefs.SERVER_PORT]) }
            }
            runCatching { dlnaRenderer.start() }
        }
        _disconnectRestart = restart
        _mainHandler.postDelayed(restart, DISCONNECT_RESTART_DELAY_MS)
        log("Casting disconnected; servers restarting in ${DISCONNECT_RESTART_DELAY_MS}ms to stay castable")
    }

    fun downloadVideo() {
        _videoLocation.value?.let { videoDownloader.start(it) }
    }

    private fun _endVideoPlayback(message: String) {
        if (!_videoPlaybackActive.value) return
        // lingering polls after a stop must not bounce the UI back to a pending session
        _videoPollSuppressed = true
        _videoPlaybackActive.value = false
        _videoSyncLowLatency(false)
        airPlayVideoPlayer.stop()
        dlnaVideoPlayer.stop()
        if (!_audioOnly.value) mediaSession?.isActive = false
        log(message)
    }

    /**
     * One receiver, one session. A mirror starting on top of a DLNA or AirPlay-URL video
     * leaves that ExoPlayer decoding its own soundtrack (two soundtracks at once) while it
     * still holds the on-screen video surface — so the new mirror renders into a surface
     * that is not on screen and nothing visibly happens. Evicting the old session fixes
     * both symptoms at once. Idempotent: no-op unless a video session is actually live.
     *
     * [claimingMirror] also raises the mirroring flag before the old session is dropped:
     * MainActivity backgrounds the app the moment both "playing" flags go false, and the
     * gap between "video stopped" and "mirror running" would push the new mirror session
     * into the background as if the cast had ended.
     */
    private fun _evictVideoPlayback(source: String, claimingMirror: Boolean = false) {
        if (!_videoPlaybackActive.value) return
        if (claimingMirror) _mirroringActive.value = true
        _endVideoPlayback("$source started: previous video session stopped")
    }

    /**
     * The mirror direction: a DLNA cast arriving while the phone is still mirroring (or
     * streaming music) would play both soundtracks. The AirPlay session is owned by the
     * sender, so the only way to end it is to take the RTSP server down; it comes back
     * shortly after, in the same pattern the back-key disconnect uses, so the device
     * stays castable and the old sender does not re-attach on top of the new cast.
     */
    private fun _evictAirPlaySession(reason: String) {
        if (nativeHandle == 0L) return
        if (!_mirroringActive.value && !_audioOnly.value && _connectionCount.value == 0) return
        log("$reason took over: dropping the AirPlay session")
        _disconnecting = true
        _lastVideoPollAt = 0
        _mirroringActive.value = false
        _videoSyncLowLatency(false)
        // 2.0: both panes go, otherwise the surviving session keeps decoding into a surface
        // that is no longer on screen while the DLNA cast owns the display
        _dropAllSessions()
        videoRenderer.stopSession()
        videoRenderer.clearDisplay()
        _setAudioOnly(false)
        audioRenderer.stop()
        mediaSession?.isActive = false
        runCatching { NativeBridge.nativeStop(nativeHandle) }
        _disconnectRestart?.let { _mainHandler.removeCallbacks(it) }
        val restart = Runnable {
            _disconnectRestart = null
            // the next sender connects to a fresh server; without clearing this the
            // pending-session poll suppression would swallow its first /playback-info
            _disconnecting = false
            if (nativeHandle != 0L) {
                runCatching { NativeBridge.nativeStart(nativeHandle, prefs[Prefs.SERVER_PORT]) }
            }
        }
        _disconnectRestart = restart
        _mainHandler.postDelayed(restart, EVICT_RESTART_DELAY_MS)
        _serverState.value = ServerState.RUNNING
    }

    override fun onDestroy() {
        stopServer()
        dacpPlayer.release()
        mediaReceiver?.let {
            try { unregisterReceiver(it) } catch (_: Exception) {}
        }
        mediaReceiver = null
        volumeReceiver?.let {
            try { unregisterReceiver(it) } catch (_: Exception) {}
        }
        volumeReceiver = null
        dacpController?.release()
        dacpController = null
        mediaSession?.release()
        mediaSession = null
        super.onDestroy()
    }

    // RaopCallbackHandler (called from native threads)

    override fun onVideoData(sessionId: Long, buf: ByteBuffer, len: Int, ntpTimeNs: Long, isH265: Boolean) {
        val s = _sessionFor(sessionId, create = true) ?: return
        s.renderer.feedFrame(buf, len, ntpTimeNs, isH265)
    }

    override fun onClientInfo(sessionId: Long, model: String, name: String) {
        val s = _sessionFor(sessionId, create = true) ?: return
        s.model = model
        s.deviceName = name
        log("Client $sessionId: $name ($model)")
        _publishPanes()
    }

    override fun onVideoSessionPoll() {
        if (_disconnecting) return
        _lastVideoPollAt = SystemClock.elapsedRealtime()
    }

    override fun onVideoPlay(location: String, startPositionSeconds: Float) {
        _disconnecting = false
        // an AirPlay URL cast replaces whatever DLNA session may still be running: its
        // ExoPlayer would otherwise keep playing audio and hold the video surface
        dlnaVideoPlayer.stop()
        _videoLocation.value = location
        _videoPlaySeq.value++
        _videoPollSuppressed = false
        _videoPlaybackInfo.value = VideoPlaybackInfo(positionMs = (startPositionSeconds * 1000).toLong())
        _videoPlaybackAspect.value = 16f / 9f
        _videoPlaybackSize.value = null
        _videoTitle.value = ""
        _videoPlaybackActive.value = true
        // keep the native RAOP audio buffer small during video so it keeps pace with the
        // ExoPlayer picture (otherwise its adaptive cushion lags audio ~1s behind)
        _videoSyncLowLatency(true)
        _dlnaOwnsVideo = false
        airPlayVideoPlayer.play(location, startPositionSeconds)
        // claim media-button routing for keys that arrive as media-session events
        mediaSession?.isActive = true
        _updateMediaNotification()
        log("AirPlay Video play: $location @ ${startPositionSeconds}s")
    }

    override fun onVideoScrub(positionSeconds: Float) {
        airPlayVideoPlayer.scrub(positionSeconds)
    }

    override fun onVideoRate(rate: Float) {
        airPlayVideoPlayer.setRate(rate)
    }

    override fun onVideoStop() = _endVideoPlayback("AirPlay Video stopped")

    override fun onAudioFormat(sessionId: Long, ct: Int, spf: Int, usingScreen: Boolean) {
        clearPin()
        val session = if (sessionId != 0L) _sessionFor(sessionId, create = true) else null
        // a sender gets exactly one shot at the soundtrack, the first time it delivers media
        session?.let { _claimAudioFor(it) }
        // the muted session keeps announcing formats (pause/resume/rotate): letting it
        // reach setFormat would rebuild the decoder under the owner mid-playback
        if (audioOwnerId != 0L && sessionId != audioOwnerId) {
            log("Audio format from muted session=$sessionId (owner=$audioOwnerId) - ignored")
            return
        }
        audioRenderer.start()
        audioRenderer.setFormat(ct, spf)
        if (!usingScreen) _setPlaying(true)
        if (!usingScreen && !_audioOnly.value) {
            // pure music streaming (not screen mirroring audio)
            _setAudioOnly(true)
        } else if (usingScreen) {
            // mirroring audio: the stream type is known here, before the first video
            // frame reports its size. pin the low-latency profile now so the adaptive
            // tuner never ramps a large cushion for this session (mirror audio must
            // track the near-instant video, not trail it by the jitter cushion)
            _evictVideoPlayback("AirPlay mirror", claimingMirror = true)
            _videoSyncLowLatency(true)
        }
        log("Audio format: session=$sessionId ct=$ct spf=$spf screen=$usingScreen")
    }

    override fun onVideoSize(sessionId: Long, srcW: Float, srcH: Float, w: Float, h: Float) {
        clearPin()
        val sw = srcW.roundToInt()
        val sh = srcH.roundToInt()
        val s = _sessionFor(sessionId, create = true)
        if (s != null && sw > 0 && sh > 0) {
            // a mirror video stream taking a real size is a new session too: if a DLNA or
            // URL cast is still running it owns the on-screen surface and its own audio
            // path, so the mirror would render nowhere and both soundtracks would play
            _evictVideoPlayback("AirPlay mirror", claimingMirror = true)
            // render the mirror at the SOURCE frame size (not the TV's 16:9) so a portrait
            // phone isn't stretched; the SurfaceView then holds the phone's real aspect and
            // letterboxes instead of distorting. On phone rotation this fires again and the
            // renderer swaps to the new orientation.
            s.renderer.setResolution(sw, sh)
            s.hasVideo = true
            s.aspect = sw.toFloat() / sh
            s.resolution = "${sw}x${sh}"
            _videoAspect.value = s.aspect
            _videoResolution.value = s.resolution
            _mirroringActive.value = true
            // a session that never announced an audio format still gets its vote here, so a
            // late joiner always ends up owning the soundtrack
            _claimAudioFor(s)
            _publishPanes()
            // mirroring video renders near-instantly via MediaCodec; pin the RAOP audio cushion
            // low too, or its adaptive buffer lags audio ~1s behind (live feeds sound late at first)
            _videoSyncLowLatency(true)
        }
        log("Video size: session=$sessionId ${srcW}x${srcH} -> ${w}x${h}")
    }

    override fun onVolumeChange(volume: Float) {
        val frac = if (volume <= -144f) 0f else ((volume + 30f) / 30f).coerceIn(0f, 1f)
        _senderFrac = frac
        Log.d(TAG, "volume ${volume}dB, frac $frac")
        _mainHandler.post {
            if (_volSyncTarget >= 0f) {
                _volSyncStep()
                return@post
            }
            val max = audioManager.getStreamMaxVolume(AudioManager.STREAM_MUSIC)
            val cur = audioManager.getStreamVolume(AudioManager.STREAM_MUSIC)
            val idx = (frac * max).roundToInt()
            _preZeroIdx = if (idx == 0) (if (_preZeroIdx < 0) cur else _preZeroIdx) else -1
            if (idx != cur) {
                _pendingVolEchoes++
                audioManager.setStreamVolume(AudioManager.STREAM_MUSIC, idx, 0)
            }
        }
    }

    override fun onClientVolume(): Float {
        val max = audioManager.getStreamMaxVolume(AudioManager.STREAM_MUSIC)
        val vol = audioManager.getStreamVolume(AudioManager.STREAM_MUSIC)
        val db = if (vol == 0) -144f else -30f + 30f * vol / max
        Log.d(TAG, "client volume query: $vol/$max -> ${db}dB")
        return db
    }

    private fun _volSyncStep() {
        _mainHandler.removeCallbacks(_volSyncTimeout)
        val target = _volSyncTarget
        if (target < 0f) return
        val frac = _senderFrac
        val want = when {
            frac < 0f -> _volSyncHint
            abs(target - frac) <= VOL_SYNC_EPS -> 0
            target > frac -> 1
            else -> -1
        }
        if (want == 0 || (_volSyncDir != 0 && want != _volSyncDir) || _volSyncSteps >= VOL_SYNC_MAX_STEPS) {
            _volSyncEnd()
            return
        }
        _volSyncDir = want
        _volSyncSteps++
        if (want > 0) dacpController?.volumeUp() else dacpController?.volumeDown()
        _mainHandler.postDelayed(_volSyncTimeout, VOL_SYNC_TIMEOUT_MS)
    }

    private fun _volSyncEnd() {
        _mainHandler.removeCallbacks(_volSyncTimeout)
        if (_volSyncTarget >= 0f) Log.d(TAG, "volume sync done: sender $_senderFrac, target $_volSyncTarget")
        _volSyncTarget = -1f
        _volSyncDir = 0
    }

    override fun onConnectionInit(sessionId: Long) {
        _disconnecting = false
        val firstConnection = _connectionCount.value == 0
        _connectionCount.value++
        log("Client connected (${_connectionCount.value})")
        if (!firstConnection) return
        // conn_init is only a tcp pre-auth signal. pin-required sessions must wait for
        // onDisplayPin, otherwise the server ui can move before the client pin is current
        if (requiresPin()) return
        if (!shouldLaunchOnConnect()) return
        launchMainActivity()
    }

    override fun onConnectionDestroy(sessionId: Long) {
        _dropSession(sessionId)
        _connectionCount.value = (_connectionCount.value - 1).coerceAtLeast(0)
        // a back-key teardown already cleared all cast state; do NOT re-flag pending / reset the
        // poll-suppression here, otherwise a lingering native poll bounces the UI back to the
        // black fullscreen right after leaving the cast
        if (_disconnecting) return
        if (_connectionCount.value == 0) {
            // clients may drop without POST /stop; must run before the poll-state reset
            _endVideoPlayback("AirPlay Video stopped (disconnect)")
            // last client gone: release audio output devices to save power
            audioRenderer.stop()
            _audioOnly.value = false
            _lastVideoPollAt = 0
            _videoPollSuppressed = false
            _coverArtBytes = null
            _trackInfo.value = TrackInfo()
            _positionMs.value = 0
            _durationMs.value = 0
            dacpController?.reset()
            _senderFrac = -1f
            _mainHandler.post {
                _volSyncEnd()
                if (_preZeroIdx >= 0) {
                    audioManager.setStreamVolume(AudioManager.STREAM_MUSIC, _preZeroIdx, 0)
                    _preZeroIdx = -1
                }
            }
            mediaSession?.isActive = false
            _refreshDacpPlayer()
            _updateMediaNotification()
        }
        log("Client disconnected (${_connectionCount.value})")
    }

    override fun onConnectionReset(sessionId: Long, reason: Int) {
        log("Connection reset: session=$sessionId reason=$reason")
    }

    override fun onDisplayPin(pin: String) {
        // a new pin is the sync point with the client prompt: show every new value immediately
        if (_lastPin == pin) return
        _lastPin = pin
        pinCallback?.invoke(pin)
        _updateMediaNotification()
    }

    override fun onMetadata(data: ByteArray) {
        val map = DmapParser.parse(data)
        val info = TrackInfo.fromDmap(map, _trackInfo.value.coverArt)
        _trackInfo.value = info
        if (info.durationMs > 0) _durationMs.value = info.durationMs
        _updateMediaMetadata()
        _refreshDacpPlayer()
        log("Track: ${info.artist} - ${info.title}")
    }

    override fun onCoverArt(data: ByteArray) {
        // covers are often 1000px+ but are only ever shown as notification /
        // media-session art: decode down to ~512px instead of full size
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeByteArray(data, 0, data.size, bounds)
        val opts = BitmapFactory.Options().apply {
            inSampleSize = _coverArtSampleSize(bounds.outWidth, bounds.outHeight)
        }
        val bmp = BitmapFactory.decodeByteArray(data, 0, data.size, opts) ?: return
        _coverArtBytes = data
        _trackInfo.value = _trackInfo.value.copy(coverArt = bmp)
        _updateMediaMetadata()
        _refreshDacpPlayer()
    }

    // largest power of two that keeps both dimensions >= COVER_ART_TARGET_PX
    private fun _coverArtSampleSize(w: Int, h: Int): Int {
        if (w <= 0 || h <= 0) return 1
        var sample = 1
        while (w / (sample * 2) >= COVER_ART_TARGET_PX && h / (sample * 2) >= COVER_ART_TARGET_PX) {
            sample *= 2
        }
        return sample
    }

    override fun onProgress(startMs: Long, currMs: Long, endMs: Long) {
        // native converts stream frames to ms at the engine's real sample rate
        val posMs = (currMs - startMs).coerceAtLeast(0)
        val durMs = (endMs - startMs).coerceAtLeast(0)
        // pause/resume transitions emit degenerate progress; keep the last good value
        if (durMs <= 0) return
        _positionMs.value = posMs
        _durationMs.value = durMs
        _progressAnchor = ProgressAnchor(posMs, SystemClock.elapsedRealtime())
        _playing.value = true
        _updatePlaybackState()
        _refreshDacpPlayer()
    }

    override fun onAudioTeardown() {
        _setPlaying(false)
    }

    override fun onDacpId(dacpId: String, activeRemote: String) {
        dacpController?.update(dacpId, activeRemote)
        log("DACP: $dacpId")
    }

    override fun onMirrorRunning(sessionId: Long, running: Boolean) {
        if (running) {
            _evictVideoPlayback("AirPlay mirror", claimingMirror = true)
            _sessionFor(sessionId, create = true)?.renderer?.startSession()
            _videoSyncLowLatency(true)
            _mirroringActive.value = true
            _publishPanes()
        } else {
            _dropSession(sessionId)
        }
        _setAudioOnly(synchronized(sessionLock) { sessions.isEmpty() } && !running)
        // a live mirror is an active media session: claim media-button routing so lock-screen
        // controls and system media keys can reach it (previously the session stayed inactive
        // during mirroring, so the OS ignored it)
        if (running) mediaSession?.isActive = true
        _updateMediaNotification()
    }

    private fun _setAudioOnly(audioOnly: Boolean) {
        val prev = _audioOnly.value
        _audioOnly.value = audioOnly
        _refreshDacpPlayer()
        if (audioOnly && !prev) {
            mediaSession?.isActive = true
            log("Audio mode")
        } else if (!audioOnly && prev) {
            mediaSession?.isActive = false
            _coverArtBytes = null
            _trackInfo.value = TrackInfo()
            _positionMs.value = 0
            _durationMs.value = 0
            _updateMediaNotification()
            log("Mirror mode")
        }
    }

    private fun _updateMediaMetadata() {
        val info = _trackInfo.value
        val builder = MediaMetadataCompat.Builder()
            .putString(MediaMetadataCompat.METADATA_KEY_TITLE, info.title)
            .putString(MediaMetadataCompat.METADATA_KEY_ARTIST, info.artist)
            .putString(MediaMetadataCompat.METADATA_KEY_ALBUM, info.album)
            .putLong(MediaMetadataCompat.METADATA_KEY_DURATION, _durationMs.value)
        info.coverArt?.let { builder.putBitmap(MediaMetadataCompat.METADATA_KEY_ALBUM_ART, it) }
        mediaSession?.setMetadata(builder.build())
        _updateMediaNotification()
    }

    fun togglePlayPause() {
        val nowPlaying = !_playing.value
        _setPlaying(nowPlaying)
        dacpController?.let { if (nowPlaying) it.play() else it.pause() }
    }

    private fun _setPlaying(playing: Boolean) {
        _playing.value = playing
        _refreshDacpPlayer()
        if (playing) {
            // resume extrapolation from current position
            _progressAnchor = ProgressAnchor(_positionMs.value, SystemClock.elapsedRealtime())
        } else {
            // freeze position
            _positionMs.value = currentPositionMs()
            _progressAnchor = null
        }
        _updatePlaybackState()
    }

    private fun _updatePlaybackState() {
        // the sender's silent raop audio session must not overwrite video session state
        if (_videoPlaybackActive.value) return
        val isPlaying = _playing.value
        val pbState = if (isPlaying) PlaybackStateCompat.STATE_PLAYING else PlaybackStateCompat.STATE_PAUSED
        val speed = if (isPlaying) 1f else 0f
        val state = PlaybackStateCompat.Builder()
            .setActions(
                PlaybackStateCompat.ACTION_PLAY or
                    PlaybackStateCompat.ACTION_PAUSE or
                    PlaybackStateCompat.ACTION_PLAY_PAUSE or
                    PlaybackStateCompat.ACTION_SKIP_TO_NEXT or
                    PlaybackStateCompat.ACTION_SKIP_TO_PREVIOUS
            )
            .setState(pbState, _positionMs.value, speed, SystemClock.elapsedRealtime())
            .build()
        mediaSession?.setPlaybackState(state)
        _updateMediaNotification()
    }

    // consumers extrapolate position from (position, speed, updateTime): push only discontinuities
    private fun _updateVideoPlaybackState(positionSeconds: Float, rate: Float) {
        val playing = rate > 0f
        val posMs = (positionSeconds * 1000).toLong()
        val now = SystemClock.elapsedRealtime()
        val expectedMs = _lastVideoStatePosMs +
            if (_lastVideoStateRate > 0f) ((now - _lastVideoStateAtMs) * _lastVideoStateRate).toLong() else 0L
        if (rate == _lastVideoStateRate && abs(posMs - expectedMs) < 1000) return
        _lastVideoStateRate = rate
        _lastVideoStatePosMs = posMs
        _lastVideoStateAtMs = now
        val state = PlaybackStateCompat.Builder()
            .setActions(
                PlaybackStateCompat.ACTION_PLAY or
                    PlaybackStateCompat.ACTION_PAUSE or
                    PlaybackStateCompat.ACTION_PLAY_PAUSE or
                    PlaybackStateCompat.ACTION_STOP or
                    PlaybackStateCompat.ACTION_FAST_FORWARD or
                    PlaybackStateCompat.ACTION_REWIND or
                    PlaybackStateCompat.ACTION_SEEK_TO
            )
            .setState(
                if (playing) PlaybackStateCompat.STATE_PLAYING else PlaybackStateCompat.STATE_PAUSED,
                posMs,
                rate,
                now
            )
            .build()
        mediaSession?.setPlaybackState(state)
        _updateMediaNotification()
    }

    private var _lastVideoStateRate = -1f
    private var _lastVideoStatePosMs = 0L
    private var _lastVideoStateAtMs = 0L

    private fun _refreshDacpPlayer() {
        _mainHandler.post { dacpPlayer.refresh() }
    }

    private fun clearPin() {
        _lastPin = null
        pinCallback?.invoke(null)
        _updateMediaNotification()
    }

    fun collectDebugInfo(): DebugInfo {
        // mirror renders through the native VideoRenderer (codec/fps/frames populated there);
        // AirPlay/DLNA video goes through ExoPlayer, whose stats live on the video player.
        // 2.0: with two panes the numbers belong to the pane that owns the audio clock.
        val r = _activeRenderer()
        return DebugInfo(
            videoCodec = if (_videoPlaybackActive.value) airPlayVideoPlayer.currentVideoFormat().codecs else r.codecName,
            videoRes = if (_videoPlaybackActive.value) {
                val f = airPlayVideoPlayer.currentVideoFormat()
                if (f.width > 0) "${f.width}x${f.height}" else _videoResolution.value
            } else _videoResolution.value,
            videoFps = if (_videoPlaybackActive.value) {
                // exoplayer: container frame rate when it carries one, else the measured render rate
                airPlayVideoPlayer.currentVideoFormat().fps.roundToInt()
            } else r.fps,
            videoBitrate = if (_videoPlaybackActive.value) airPlayVideoPlayer.currentVideoFormat().bitrateBps else r.bitrateBps,
            videoFrames = if (_videoPlaybackActive.value) 0 else r.frameCount,
            droppedFrames = if (_videoPlaybackActive.value) 0 else r.droppedFrames,
            framePacingJitterUs = if (_videoPlaybackActive.value) 0 else r.framePacingJitterUs,
            audioCodec = audioRenderer.codecLabel,
            audioVolume = 100 * audioManager.getStreamVolume(AudioManager.STREAM_MUSIC) /
                audioManager.getStreamMaxVolume(AudioManager.STREAM_MUSIC),
            audio = audioRenderer.audioDebug(),
            connections = _connectionCount.value,
        )
    }

    // helpers

    private fun getHwAddr(): ByteArray {
        try {
            val interfaces = NetworkInterface.getNetworkInterfaces()
            for (iface in interfaces) {
                if (iface.name.startsWith("wlan") || iface.name.startsWith("eth")) {
                    val mac = iface.hardwareAddress
                    if (isUsableMac(mac)) return mac!!
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "Failed to get hardware address", e)
        }

        // fall back to stable per-install random address
        return persistedRandomMac()
            ?: byteArrayOf(0xAA.toByte(), 0xBB.toByte(), 0xCC.toByte(), 0xDD.toByte(), 0xEE.toByte(), 0xFF.toByte())
    }

    private fun isUsableMac(mac: ByteArray?): Boolean =
        mac != null && mac.size == 6 &&
            mac.any { it != 0.toByte() } &&
            !(mac[0] == 0x02.toByte() && mac.drop(1).all { it == 0.toByte() })

    private fun persistedRandomMac(): ByteArray? {
        macFromString(prefs.getString(Prefs.FALLBACK_MAC_ADDRESS, null))
            ?.takeIf { isUsableMac(it) }?.let { return it }
        repeat(10) {
            val mac = randomAaiMac()
            if (isUsableMac(mac)) {
                prefs.edit().putString(Prefs.FALLBACK_MAC_ADDRESS, macToString(mac)).apply()
                return mac
            }
        }
        return null
    }

    // random locally-administered unicast MAC in AAI SLAP quadrant
    private fun randomAaiMac(): ByteArray {
        val mac = ByteArray(6).also { SecureRandom().nextBytes(it) }
        mac[0] = ((mac[0].toInt() and 0xF0) or 0x0A).toByte()
        return mac
    }

    private fun macToString(mac: ByteArray): String = mac.joinToString(":") { "%02x".format(it) }

    private fun macFromString(s: String?): ByteArray? {
        if (s == null) return null
        return try {
            s.split(":").map { it.toInt(16).toByte() }.toByteArray()
        } catch (e: Exception) { null }
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val nm = getSystemService(NOTIFICATION_SERVICE) as NotificationManager
        // The 2026-10-21 "silent" experiment (IMPORTANCE_MIN + transparent icon, hidden
        // status-bar presence) was rolled back at the user's request: a normal, visible
        // icon is preferred. Drop that channel so the id below is recreated with normal
        // importance on devices that already ran the silent build — a channel's importance
        // can only be lowered by the user, never raised in code.
        nm.deleteNotificationChannel(SILENT_CHANNEL_ID)
        val channel = NotificationChannel(
            CHANNEL_ID,
            getString(R.string.notification_channel),
            // LOW: silent (no sound/vibration/peek) but the status-bar icon stays
            // visible, like every other well-behaved background service.
            NotificationManager.IMPORTANCE_LOW
        ).apply {
            setShowBadge(false)
            enableVibration(false)
            setSound(null, null)
        }
        nm.createNotificationChannel(channel)
    }

    private fun buildNotification(): Notification {
        return _buildMediaNotification()
    }

    private fun promoteToForeground() {
        ServiceCompat.startForeground(
            this,
            NOTIFICATION_ID,
            buildNotification(),
            ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
        )
        foregroundStarted = true
    }

    private fun requiresPin(): Boolean {
        return prefs[Prefs.REQUIRE_PIN]
    }

    private fun shouldLaunchOnConnect(): Boolean {
        return prefs[Prefs.LAUNCH_ON_CONNECT]
    }

    private fun _buildMediaNotification(): Notification {
        val intent = Intent(this, MainActivity::class.java)
            .putExtra(MainActivity.EXTRA_FROM_NOTIFICATION, true)
        val pi = PendingIntent.getActivity(this, 1, intent, PendingIntent.FLAG_IMMUTABLE)
        val info = _trackInfo.value
        val isAudio = _audioOnly.value && info.title.isNotEmpty()
        val isVideo = _videoPlaybackActive.value
        val isMirror = _mirroringActive.value
        // show transport controls whenever a media session is actually playing something
        val hasTransport = isAudio || isVideo || isMirror

        val builder = NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_stat_cast)
            .setContentIntent(pi)
            .setOngoing(true)

        when {
            isAudio -> {
                builder.setContentTitle(info.title).setContentText(info.artist).setSubText(info.album)
                info.coverArt?.let { builder.setLargeIcon(it) }
            }
            isVideo -> {
                builder.setContentTitle(_videoTitle.value.ifEmpty { getString(R.string.notification_video_title) })
                    .setContentText(getString(R.string.notification_video_text))
            }
            isMirror -> {
                builder.setContentTitle(getString(R.string.notification_mirror_title))
                    .setContentText(getString(R.string.notification_mirror_text))
            }
            _lastPin != null -> {
                // passive handoff only: do not launch/reorder the activity during pin auth
                builder.setContentTitle(getString(R.string.notification_pin_title))
                    .setContentText(getString(R.string.notification_pin_text, _lastPin))
            }
            else -> {
                builder.setContentTitle(getString(R.string.notification_title))
                    .setContentText(getString(R.string.notification_text))
            }
        }

        if (hasTransport) {
            mediaSession?.sessionToken?.let { token ->
                builder.setStyle(
                    MediaNotificationCompat.MediaStyle()
                        .setMediaSession(token)
                        .setShowActionsInCompactView(0, 1, 2)
                )
                // audio and video both have meaningful play/pause + skip controls; mirror is a
                // passive session (it surfaces on the lock screen via the active MediaSession) but
                // has no play/pause/next semantics of its own
                if (isAudio || isVideo) {
                    builder.addAction(android.R.drawable.ic_media_previous, "Prev", _mediaAction(ACTION_PREV))
                    builder.addAction(android.R.drawable.ic_media_pause, "Pause", _mediaAction(ACTION_PLAY_PAUSE))
                    builder.addAction(android.R.drawable.ic_media_next, "Next", _mediaAction(ACTION_NEXT))
                }
            }
        }
        return builder.build()
    }

    private fun launchMainActivity() {
        Handler(Looper.getMainLooper()).post {
            val launchIntent = Intent(this, MainActivity::class.java)
                .putExtra(MainActivity.EXTRA_FROM_CAST, true)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_REORDER_TO_FRONT)
            try {
                startActivity(launchIntent)
            } catch (e: Exception) {
                Log.w(TAG, "Failed to launch activity", e)
            }
        }
    }

    private fun _mediaAction(action: String): PendingIntent {
        val intent = Intent(action).setPackage(packageName)
        return PendingIntent.getBroadcast(
            this,
            action.hashCode(),
            intent,
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )
    }

    private fun _updateMediaNotification() {
        if (_serverState.value != ServerState.RUNNING) return
        val nm = getSystemService(NOTIFICATION_SERVICE) as NotificationManager
        nm.notify(NOTIFICATION_ID, _buildMediaNotification())
    }

    enum class ServerState {
        STOPPED,
        RUNNING,
        ERROR
    }

    companion object {
        private const val TAG = "AirPlayService"
        private const val VOL_SYNC_EPS = 0.033f
        private const val VOL_SYNC_MAX_STEPS = 32
        private const val VOL_SYNC_TIMEOUT_MS = 800L
        private const val CHANNEL_ID = "airplay_service"
        private const val SILENT_CHANNEL_ID = "airplay_service_silent"
        private const val NOTIFICATION_ID = 1
        const val ACTION_PLAY_PAUSE = "io.github.jqssun.airplay.PLAY_PAUSE"
        const val ACTION_NEXT = "io.github.jqssun.airplay.NEXT"
        const val ACTION_PREV = "io.github.jqssun.airplay.PREV"
        const val ACTION_START_SERVER = "io.github.jqssun.airplay.START_SERVER"

        /**
         * Process-global mirror of the server state, readable before MainActivity binds the
         * service (the boot auto-start guard in MainActivity needs it in onCreate).
         */
        @Volatile
        var isServerRunning: Boolean = false
            private set
        // shared with dpad/double-tap seeks
        const val VIDEO_SEEK_STEP_MS = 10_000L
        const val VIDEO_POLL_PENDING_TIMEOUT_MS = 3_000L

        private const val AUDIO_CONFIG_DEBOUNCE_MS = 500L
        private const val SERVER_RESTART_DEBOUNCE_MS = 500L
        // cover art is decoded at down to this size (notification / media session)
        private const val COVER_ART_TARGET_PX = 512
    }
}
