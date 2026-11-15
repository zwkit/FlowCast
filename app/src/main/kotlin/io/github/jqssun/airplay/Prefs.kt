package io.github.jqssun.airplay

import android.content.SharedPreferences
import android.media.MediaFormat
import android.os.Build
import androidx.core.content.edit
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.onStart

sealed class Pref<T : Any>(val key: String, val def: T, val restart: Boolean) {
    abstract fun parse(s: String): T?
    open fun valid(v: T) = true
    protected abstract fun SharedPreferences.load(): T
    protected abstract fun SharedPreferences.Editor.store(v: T)

    fun read(p: SharedPreferences): T = p.load().takeIf(::valid) ?: def
    fun write(p: SharedPreferences, v: T): Boolean = valid(v).also { if (it) p.edit { store(v) } }
}

class BoolPref(key: String, def: Boolean, restart: Boolean) : Pref<Boolean>(key, def, restart) {
    override fun parse(s: String) = s.toBooleanStrictOrNull()
    override fun SharedPreferences.load() = getBoolean(key, def)
    override fun SharedPreferences.Editor.store(v: Boolean) { putBoolean(key, v) }
}

class IntPref(key: String, def: Int, val range: IntRange, restart: Boolean) : Pref<Int>(key, def, restart) {
    override fun parse(s: String) = s.toIntOrNull()
    override fun valid(v: Int) = v in range
    override fun SharedPreferences.load() = getInt(key, def)
    override fun SharedPreferences.Editor.store(v: Int) { putInt(key, v) }
}

class StrPref(key: String, def: String, restart: Boolean, private val check: (String) -> Boolean) : Pref<String>(key, def, restart) {
    override fun parse(s: String) = s
    override fun valid(v: String) = check(v)
    override fun SharedPreferences.load() = getString(key, def) ?: def
    override fun SharedPreferences.Editor.store(v: String) { putString(key, v) }
}

operator fun <T : Any> SharedPreferences.get(p: Pref<T>): T = p.read(this)

fun SharedPreferences.changes(): Flow<String> = callbackFlow {
    val listener = SharedPreferences.OnSharedPreferenceChangeListener { _, key -> if (key != null) trySend(key) }
    registerOnSharedPreferenceChangeListener(listener)
    awaitClose { unregisterOnSharedPreferenceChangeListener(listener) }
}

fun <T> SharedPreferences.observe(read: SharedPreferences.() -> T): Flow<T> =
    changes().map { read() }.onStart { emit(read()) }.distinctUntilChanged()

object Prefs {
    const val NAME = "settings"
    const val AUTO = "auto"; const val ON = "on"; const val OFF = "off"
    const val PORTRAIT = "portrait"; const val LANDSCAPE = "landscape"
    // 2.1: UI background. It used to follow the system, which on a TV means an unpredictable
    // look (and Material You pulls the palette from the wallpaper). Dark stays the default.
    const val THEME_DARK = "dark"; const val THEME_LIGHT = "light"
    const val FALLBACK_MAC_ADDRESS = "fallback_mac_address"
    // random per install, never user-visible: the AirPlay pairing identity iOS keys on
    const val AIRPLAY_PAIRING_ID = "airplay_pairing_id"
    // random per install: the UPnP UDN of the DLNA renderer. It used to be derived from the
    // package name, so every install advertised the same UDN; a DLNA controller then sees
    // two receivers as one device and may drive whichever answered last
    const val DLNA_UDN = "dlna_udn"
    val ADAPTIVE_PERCENTILES = intArrayOf(80, 85, 90, 95, 99)
    private val SIZE = Regex("[1-9]\\d{0,4}x[1-9]\\d{0,4}")

    private val _all = mutableListOf<Pref<*>>()
    val all: Map<String, Pref<*>> by lazy { _all.associateBy { it.key } }

    private fun bool(key: String, def: Boolean, restart: Boolean = false) =
        BoolPref(key, def, restart).also(_all::add)
    private fun int(key: String, def: Int, range: IntRange, restart: Boolean = false) =
        IntPref(key, def, range, restart).also(_all::add)
    private fun str(key: String, def: String, restart: Boolean = false, check: (String) -> Boolean = { true }) =
        StrPref(key, def, restart, check).also(_all::add)

    // app UI language: "system" | "zh-rCN" | "en"; defaults to Simplified Chinese
    val APP_LANGUAGE = str("app_language", "zh-rCN")
    // the device's own name, used as the default server name
    val defaultDeviceName: String = "${Build.MANUFACTURER} ${Build.MODEL}".trim()
    // server name: empty means "use the device's own name as read from the system"
    val SERVER_NAME = str("server_name", "", restart = true)
    val SERVER_PORT = int("server_port", 7000, 1..65535, restart = true)
    val AUTO_START = bool("auto_start", true)
    val BOOT_AUTO_START = bool("boot_auto_start", true)
    val RUN_IN_BACKGROUND = bool("run_in_background", true)
    val H265_ENABLED = bool("h265_enabled", true, restart = true)
    val ENFORCE_SDR = bool("enforce_sdr", true, restart = true)
    val KEY_ALLOW_FRAME_DROP = bool(MediaFormat.KEY_ALLOW_FRAME_DROP, true, restart = true)
    val KEY_PRIORITY = bool(MediaFormat.KEY_PRIORITY, true, restart = true)
    val LOW_LATENCY = bool("low_latency", false)
    val OPERATING_RATE = str("operating_rate", AUTO, restart = true) { it in setOf(AUTO, ON, OFF) }
    val SCHEDULED_OUTPUT_BUFFER_RELEASE = bool("scheduled_output_buffer_release", true, restart = true)
    // A/V sync: the video timeline is aligned to the audio playout clock by
    // default; this nudges the picture relative to it for hardware whose decoded
    // A/V latency differs. negative advances the picture, positive delays it
    val AV_SYNC_OFFSET_MS = int("av_sync_offset_ms", 0, -500..500)
    val AUDIO_AUTO_BUFFER = bool("audio_auto_buffer", true)
    // fixed cushion ms, used only when AUDIO_AUTO_BUFFER is off
    val AUDIO_CUSHION_MS = int("audio_cushion_ms", 40, 1..1000)
    // slider step 0..4 mapping to arrival-delay percentile the cushion targets;
    // lower = less latency, higher = more stable
    val AUDIO_ADAPTIVE_STEP = int("audio_adaptive_step", 3, ADAPTIVE_PERCENTILES.indices)
    val OBOE_BUFFER_FRAMES = int("oboe_buffer_frames", 0, 0..8192)
    val ALAC_ENABLED = bool("alac_enabled", false, restart = true)
    val FORCE_SW_ALAC = bool("force_sw_alac", true)
    val FORCE_SW_AAC = bool("force_sw_aac", false)
    val AAC_ENABLED = bool("aac_enabled", true, restart = true)
    val RESOLUTION = str("resolution", AUTO, restart = true) { it in setOf(AUTO, PORTRAIT, LANDSCAPE) || SIZE.matches(it) }
    val MAX_FPS = int("max_fps", 60, 1..Int.MAX_VALUE, restart = true)
    val OVERSCANNED = bool("overscanned", false, restart = true)
    val REQUIRE_PIN = bool("require_pin", false, restart = true)
    val ALLOW_NEW_CONN = bool("allow_new_conn", true, restart = true)
    // 2.0: accept up to two AirPlay mirror sessions at once (two panes on one screen)
    val MULTI_SESSION = bool("multi_session", true, restart = true)
    // 2.1: LAN page behind a QR code on the TV that takes a video URL from a phone
    val WEB_PUSH = bool("web_push", true, restart = true)
    val WEB_PUSH_PORT = int("web_push_port", 8061, 1024..65535, restart = true)
    // 2.0: publish synthetic mirror panes so the dual-pane layout can be checked on an
    // emulator with no iOS device around. "pp" | "ll" | "pl" | "ipad"; empty = off
    val DEBUG_PANES = str("debug_panes", "")
    val AUDIO_LATENCY_MS = int("audio_latency_ms", -1, -1..1000, restart = true)
    val DEBUG_ENABLED = bool("debug_enabled", false)
    val DEVELOPER_OPTIONS = bool("developer_options", false)
    val BENCHMARK_LOG = bool("benchmark_log", false, restart = true)
    val IDLE_PREVIEW = bool("idle_preview", false)
    val AUTO_FULLSCREEN = bool("auto_fullscreen", true)
    val THEME = str("theme", THEME_DARK) { it in setOf(THEME_DARK, THEME_LIGHT) }
    val KEEP_SCREEN_ON = bool("keep_screen_on", true)
    val ADVERTISE_VIDEO = bool("advertise_video", true, restart = true)
    val ADVERTISE_MIRRORING = bool("advertise_mirroring", true, restart = true)
    val ADVERTISE_AUDIO = bool("advertise_audio", true, restart = true)
    val LAUNCH_ON_CONNECT = bool("launch_on_connect", true)
    // 2.1: what happens when a cast ends on its own (sender disconnects, or playback runs
    // out) rather than by the user pressing back. Default keeps the seamless-cast design:
    // the receiver gets out of the way. Turned off, the receiver stays on its home page,
    // which is what you want when the idle QR code matters more than invisibility.
    val EXIT_ON_CAST_END = bool("exit_on_cast_end", true)
    val USB_ONLY = bool("usb_only", false, restart = true)
    // Advertise AirPlay through NsdManager instead of the app's own mDNS responder.
    // NsdManager publishes the *system* mDNS hostname, which on Android is shared
    // ("Android.local", "Android-2.local", ...): with a second receiver on the same LAN the
    // name can move between devices across restarts, and the phone's cached SRV -> A then
    // sends it to the wrong one. Kept as an escape hatch only.
    val MDNS_LEGACY_NSD = bool("mdns_legacy_nsd", false, restart = true)
}
