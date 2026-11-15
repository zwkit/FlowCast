package io.github.jqssun.airplay.upnp

/**
 * Serves the DLNA device description and SOAP actions.
 *
 * The socket/parsing work lives in [MiniHttpServer] so the web-push page can reuse it; this
 * class stays as the DLNA-specific entry point so call sites are unchanged.
 */
internal class DlnaHttpServer(
    port: Int,
    requestHandler: (method: String, path: String, headers: Map<String, String>, body: String) -> HttpResponse,
) {
    private val server = MiniHttpServer(port, "dlna", requestHandler)

    fun start() = server.start()

    fun stop() = server.stop()
}

internal class HttpResponse(
    val statusCode: Int,
    val statusText: String,
    val contentType: String,
    val body: String,
    val headers: Map<String, String>? = null,
)
