package io.github.jqssun.airplay.webpush

import io.github.jqssun.airplay.StreamUrl
import io.github.jqssun.airplay.upnp.HttpResponse
import io.github.jqssun.airplay.upnp.MiniHttpServer
import java.net.URLDecoder
import java.nio.charset.StandardCharsets

/**
 * LAN-only page that lets a phone hand a video URL to the receiver.
 *
 * Security model: the TV shows a QR code carrying a per-session random token; every request must
 * present it. Without that, anything on the same Wi-Fi could hijack the screen. The token is
 * regenerated on every service start, so an old screenshot of the QR code stops working.
 */
internal class WebPushServer(
    private val preferredPort: Int,
    private val token: String,
    private val handler: Handler,
) {
    internal interface Handler {
        fun play(url: String)
        fun control(action: String, positionSeconds: Float)
        fun snapshot(): Snapshot
    }

    internal data class Snapshot(
        val active: Boolean,
        val url: String,
        val title: String,
        val playing: Boolean,
        val positionSeconds: Float,
        val durationSeconds: Float,
    )

    var port: Int = 0
        private set

    private var server: MiniHttpServer? = null

    /** Tries [preferredPort] and the next few, so a busy port does not kill the feature. */
    fun start(): Boolean {
        if (token.isEmpty()) return false
        for (candidate in preferredPort until preferredPort + PORT_ATTEMPTS) {
            val s = MiniHttpServer(candidate, "webpush", ::handle)
            if (runCatching { s.start() }.isSuccess) {
                server = s
                port = candidate
                return true
            }
        }
        return false
    }

    fun stop() {
        server?.stop()
        server = null
    }

    private fun handle(
        method: String,
        path: String,
        headers: Map<String, String>,
        body: String,
    ): HttpResponse {
        val split = path.split('?', limit = 2)
        val route = split[0]
        val query = parseQuery(split.getOrNull(1).orEmpty())

        // browsers ask for this unprompted and without our query string
        if (route == "/favicon.ico") return HttpResponse(204, "No Content", "text/plain", "")

        if (query["k"] != token) {
            return HttpResponse(403, "Forbidden", "text/plain; charset=utf-8", "链接已失效，请重新扫码")
        }

        return when {
            route == "/" || route == "/index.html" -> HttpResponse(
                200, "OK", "text/html; charset=utf-8", pushPage(),
                mapOf("Cache-Control" to "no-store"),
            )

            method == "POST" && route == "/api/play" -> {
                val url = parseQuery(body)["url"]?.trim().orEmpty()
                // one shared verdict for "can the player be pointed at this at all"; the phone
                // is the only place the reason can be shown, the TV screen just stays idle
                val reason = StreamUrl.rejectReason(url)
                if (reason != null) plain(400, "Bad Request", reason) else {
                    handler.play(url)
                    plain(200, "OK", "ok")
                }
            }

            method == "POST" && route == "/api/ctl" -> {
                handler.control(query["a"].orEmpty(), query["p"]?.toFloatOrNull() ?: -1f)
                plain(200, "OK", "ok")
            }

            route == "/api/state" -> {
                val s = handler.snapshot()
                HttpResponse(
                    200, "OK", "application/json; charset=utf-8",
                    "{\"active\":${s.active},\"url\":\"${esc(s.url)}\",\"title\":\"${esc(s.title)}\"," +
                        "\"playing\":${s.playing}," +
                        "\"pos\":${s.positionSeconds},\"dur\":${s.durationSeconds}}",
                    mapOf("Cache-Control" to "no-store"),
                )
            }

            else -> plain(404, "Not Found", "not found")
        }
    }

    private fun plain(code: Int, text: String, body: String) =
        HttpResponse(code, text, "text/plain; charset=utf-8", body, mapOf("Cache-Control" to "no-store"))

    private fun parseQuery(raw: String): Map<String, String> {
        if (raw.isEmpty()) return emptyMap()
        val out = LinkedHashMap<String, String>()
        for (pair in raw.split('&')) {
            if (pair.isEmpty()) continue
            val idx = pair.indexOf('=')
            val key = if (idx < 0) pair else pair.substring(0, idx)
            val value = if (idx < 0) "" else pair.substring(idx + 1)
            runCatching {
                out[URLDecoder.decode(key, StandardCharsets.UTF_8.name())] =
                    URLDecoder.decode(value, StandardCharsets.UTF_8.name())
            }
        }
        return out
    }

    private fun esc(s: String): String = s
        .replace("\\", "\\\\")
        .replace("\"", "\\\"")
        .replace("\n", " ")
        .replace("\r", " ")

    private companion object {
        const val PORT_ATTEMPTS = 10
    }
}
