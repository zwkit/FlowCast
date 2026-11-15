package io.github.jqssun.airplay

import io.github.jqssun.airplay.stream.RtmpUrl
import io.github.jqssun.airplay.stream.UdpUrl

/**
 * Decides what a URL handed over by DLNA or the push page is, and whether the player can be
 * pointed at it at all.
 *
 * Android-free on purpose: the HTTP server must answer 400 *before* the player is touched (the
 * phone is the only place a user learns why nothing happened — the TV just stays on the idle
 * page), while the player itself has to pick a matching MediaSource. One verdict serves both.
 */
object StreamUrl {

    /** Which receiving path a URL takes. */
    enum class Kind {
        /** progressive mp4 / m3u8 / http-flv — media3's default media source */
        HTTP,
        /** camera and NVR streams, via the vendored media3 RTSP module */
        RTSP,
        /** rtmp:// and rtmps://, via the in-tree RTMP client */
        RTMP,
        /** MPEG-TS over datagrams, straight off the socket */
        UDP,
        /** the same, with an RTP header per datagram */
        RTP,
        /** anything else: never reaches the player */
        OTHER,
    }

    private val SCHEME = Regex("^[A-Za-z][A-Za-z0-9+.\\-]*$")

    /** Lowercased scheme of [raw], or "" when it has none or the prefix is not a scheme. */
    fun schemeOf(raw: String): String {
        val s = raw.trim()
        val colon = s.indexOf(':')
        if (colon <= 0) return ""
        // "host:8080/a.mp4" and "/sdcard/a.mp4" must not read as a scheme
        val firstSep = s.indexOfFirst { it == '/' || it == '?' || it == '#' }
        if (firstSep in 0 until colon) return ""
        val candidate = s.substring(0, colon)
        return if (SCHEME.matches(candidate)) candidate.lowercase() else ""
    }

    fun kindOf(raw: String): Kind = when (schemeOf(raw)) {
        "http", "https" -> Kind.HTTP
        "rtsp" -> Kind.RTSP
        "rtmp", "rtmps" -> Kind.RTMP
        "udp" -> Kind.UDP
        "rtp" -> Kind.RTP
        else -> Kind.OTHER
    }

    /** Cameras and NVRs speak this; playing it needs the vendored media3 RTSP module. */
    fun isRtsp(raw: String): Boolean = kindOf(raw) == Kind.RTSP

    /** Datagram and RTMP sources have no seekable timeline: the player buffers for latency. */
    fun isLiveStream(raw: String): Boolean = when (kindOf(raw)) {
        Kind.RTMP, Kind.UDP, Kind.RTP, Kind.RTSP -> true
        else -> false
    }

    /** Null when [raw] may go to the player, otherwise the sentence the phone shows. */
    fun rejectReason(raw: String): String? {
        val url = raw.trim()
        if (url.isEmpty()) return "缺少链接"
        val scheme = schemeOf(url)
        if (scheme.isEmpty()) return "链接要以 http:// 或 rtsp:// 开头"
        when (scheme) {
            // media3's RTSP client opens a plain socket, so a TLS variant can only fail later —
            // say it here, where the user can still fix the link. rtmps is fine: the in-tree RTMP
            // client wraps its own socket in TLS.
            "rtsps" -> return "暂不支持加密 RTSP（rtsps），请改用 rtsp://"
            "http", "https", "rtsp", "rtmp", "rtmps", "udp", "rtp" -> Unit
            // file:// and friends would play local files of the TV, which the page must not expose
            else -> return "只支持 http(s) 直链、rtsp / rtmp / rtp / udp 流"
        }
        val rest = url.substring(url.indexOf(':') + 1)
        if (!rest.startsWith("//")) return "链接不完整：缺少 //"
        when (kindOf(url)) {
            // a datagram URL may omit the host (udp://:5004 receives from anywhere), so only the
            // port is mandatory here
            Kind.UDP, Kind.RTP ->
                if (UdpUrl.parse(url) == null) {
                    return "rtp/udp 链接要带端口，例如 udp://192.168.0.9:5004"
                }
            Kind.RTMP -> {
                val ep = RtmpUrl.parse(url)
                if (ep == null || ep.streamName.isEmpty()) {
                    return "rtmp 链接要写成 rtmp://地址:1935/应用名/串流名"
                }
            }
            Kind.HTTP, Kind.RTSP ->
                if (hostOf(rest).isBlank()) return "链接不完整：缺少地址"
            else -> Unit
        }
        return null
    }

    /** Host part with credentials and port stripped, so "rtsp://:554" counts as incomplete. */
    private fun hostOf(afterColon: String): String {
        val authority = afterColon.removePrefix("//")
            .substringBefore('/')
            .substringBefore('?')
            .substringBefore('#')
            .substringAfterLast('@')
        return if (authority.startsWith("[")) {
            authority.substringBefore(']').trim('[', ']')
        } else {
            authority.substringBefore(':')
        }
    }
}
