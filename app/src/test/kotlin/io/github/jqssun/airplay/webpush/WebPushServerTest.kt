package io.github.jqssun.airplay.webpush

import java.io.BufferedInputStream
import java.net.HttpURLConnection
import java.net.URL
import kotlin.random.Random
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/**
 * End-to-end checks over a real loopback socket: the whole point of this server is the HTTP
 * contract the phone page relies on, and a mocked handler would not catch a broken route,
 * a missing token check or a body that never gets parsed.
 */
class WebPushServerTest {
    private lateinit var server: WebPushServer
    private val played = mutableListOf<String>()
    private val controls = mutableListOf<Pair<String, Float>>()
    private var snapshot = WebPushServer.Snapshot(true, "http://host/a.mp4", "T", true, 12f, 60f)

    private val token = "tok123"

    @Before
    fun setUp() {
        server = WebPushServer(
            preferredPort = 18000 + Random.nextInt(600),
            token = token,
            handler = object : WebPushServer.Handler {
                override fun play(url: String) {
                    played += url
                }

                override fun control(action: String, positionSeconds: Float) {
                    controls += action to positionSeconds
                }

                override fun snapshot(): WebPushServer.Snapshot = snapshot
            },
        )
        assertTrue("server must bind a port", server.start())
    }

    @After
    fun tearDown() {
        server.stop()
    }

    @Test
    fun `page is served only with the token`() {
        val ok = request("GET", "/?k=$token")
        assertEquals(200, ok.first)
        assertTrue(ok.second.contains("推送视频"))

        val denied = request("GET", "/?k=wrong")
        assertEquals(403, denied.first)
    }

    @Test
    fun `play accepts a form encoded url`() {
        val resp = request(
            "POST", "/api/play?k=$token",
            body = "url=" + java.net.URLEncoder.encode("https://example.com/a.mp4", "UTF-8"),
        )
        assertEquals(200, resp.first)
        assertEquals(listOf("https://example.com/a.mp4"), played)
    }

    @Test
    fun `play accepts rtsp urls`() {
        val rtsp = "rtsp://admin:secret@192.168.0.9:554/Streaming/Channels/101"
        val resp = request(
            "POST", "/api/play?k=$token",
            body = "url=" + java.net.URLEncoder.encode(rtsp, "UTF-8"),
        )
        assertEquals(200, resp.first)
        assertEquals(listOf(rtsp), played)
    }

    @Test
    fun `play rejects non http schemes`() {
        val resp = request(
            "POST", "/api/play?k=$token",
            body = "url=" + java.net.URLEncoder.encode("file:///sdcard/a.mp4", "UTF-8"),
        )
        assertEquals(400, resp.first)
        assertTrue(played.isEmpty())
    }

    @Test
    fun `play refuses rtsps instead of letting the player time out`() {
        val resp = request(
            "POST", "/api/play?k=$token",
            body = "url=" + java.net.URLEncoder.encode("rtsps://cam:322/live", "UTF-8"),
        )
        assertEquals(400, resp.first)
        assertTrue(resp.second.contains("rtsps"))
        assertTrue(played.isEmpty())
    }

    @Test
    fun `control actions reach the service`() {
        request("POST", "/api/ctl?k=$token&a=pause")
        request("POST", "/api/ctl?k=$token&a=seek&p=42.5")
        assertEquals(listOf("pause" to -1f, "seek" to 42.5f), controls)
    }

    @Test
    fun `state reports the current snapshot`() {
        val resp = request("GET", "/api/state?k=$token")
        assertEquals(200, resp.first)
        assertTrue(resp.second.contains("\"active\":true"))
        assertTrue(resp.second.contains("\"pos\":12.0"))
        assertTrue(resp.second.contains("\"dur\":60.0"))
        assertTrue(resp.second.contains("\"url\":\"http://host/a.mp4\""))
    }

    private fun request(method: String, path: String, body: String? = null): Pair<Int, String> {
        val conn = URL("http://127.0.0.1:${server.port}$path").openConnection() as HttpURLConnection
        conn.requestMethod = method
        conn.connectTimeout = 3000
        conn.readTimeout = 3000
        if (body != null) {
            conn.doOutput = true
            conn.setRequestProperty("Content-Type", "application/x-www-form-urlencoded")
            conn.outputStream.use { it.write(body.toByteArray(Charsets.UTF_8)) }
        }
        val code = conn.responseCode
        val stream = if (code in 200..299) conn.inputStream else conn.errorStream
        val text = stream?.let { BufferedInputStream(it).readBytes().toString(Charsets.UTF_8) } ?: ""
        conn.disconnect()
        return code to text
    }
}
