package io.github.jqssun.airplay.upnp

import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.nio.charset.StandardCharsets
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Minimal single-purpose HTTP server used by both the DLNA renderer (device description and
 * SOAP actions, port 8060) and the web-push page (link submission, port 8061).
 *
 * Deliberately hand-rolled: the app ships no HTTP framework, and both call sites only need
 * "parse one request, hand it to a lambda, write one response".
 */
internal class MiniHttpServer(
    private val port: Int,
    private val name: String = "http",
    private val requestHandler: (method: String, path: String, headers: Map<String, String>, body: String) -> HttpResponse,
) {
    private var serverSocket: ServerSocket? = null
    private var thread: Thread? = null
    private val running = AtomicBoolean(false)

    fun start() {
        if (running.getAndSet(true)) return
        serverSocket = ServerSocket(port, 50, InetAddress.getByName("0.0.0.0"))
        thread = Thread({ acceptLoop() }, "$name-http").also { it.isDaemon = true; it.start() }
    }

    fun stop() {
        running.set(false)
        runCatching { serverSocket?.close() }
        thread?.interrupt()
        thread = null
        serverSocket = null
    }

    private fun acceptLoop() {
        while (running.get()) {
            val socket = try { serverSocket?.accept() ?: return } catch (e: Exception) {
                if (running.get()) continue else return
            }
            Thread({ handle(socket) }, "$name-http-conn").also { it.isDaemon = true; it.start() }
        }
    }

    private fun handle(socket: Socket) {
        try {
            socket.soTimeout = 15_000
            val input = BufferedInputStream(socket.getInputStream())
            val requestLine = readLine(input) ?: return
            val parts = requestLine.split(' ')
            if (parts.size < 3) return
            val method = parts[0]
            val path = parts[1]
            val headers = LinkedHashMap<String, String>()
            var line = readLine(input)
            while (line != null && line.isNotEmpty()) {
                val idx = line.indexOf(':')
                if (idx > 0) headers[line.substring(0, idx).trim().lowercase()] = line.substring(idx + 1).trim()
                line = readLine(input)
            }
            val contentLength = headers["content-length"]?.toIntOrNull() ?: 0
            val body = if (contentLength > 0) readBody(input, contentLength) else ""
            val resp = requestHandler(method, path, headers, body)
            writeResponse(socket, resp)
        } catch (_: Exception) {
            // connection-level failures are non-fatal
        } finally {
            runCatching { socket.close() }
        }
    }

    private fun readLine(input: BufferedInputStream): String? {
        val sb = StringBuilder()
        var c = input.read()
        while (c != -1 && c != '\n'.code) {
            if (c != '\r'.code) sb.append(c.toChar())
            c = input.read()
        }
        return if (c == -1 && sb.isEmpty()) null else sb.toString()
    }

    private fun readBody(input: BufferedInputStream, length: Int): String {
        val buf = ByteArray(length)
        var off = 0
        while (off < length) {
            val n = input.read(buf, off, length - off)
            if (n < 0) break
            off += n
        }
        return String(buf, 0, off, StandardCharsets.UTF_8)
    }

    private fun writeResponse(socket: Socket, resp: HttpResponse) {
        val out = BufferedOutputStream(socket.getOutputStream())
        val bodyBytes = resp.body.toByteArray(StandardCharsets.UTF_8)
        val header = StringBuilder()
            .append("HTTP/1.1 ").append(resp.statusCode).append(' ').append(resp.statusText).append("\r\n")
            .append("Content-Type: ").append(resp.contentType).append("\r\n")
            .append("Content-Length: ").append(bodyBytes.size).append("\r\n")
        resp.headers?.forEach { (k, v) -> header.append(k).append(": ").append(v).append("\r\n") }
        header.append("Connection: close\r\n\r\n")
        out.write(header.toString().toByteArray(StandardCharsets.UTF_8))
        out.write(bodyBytes)
        out.flush()
    }
}
