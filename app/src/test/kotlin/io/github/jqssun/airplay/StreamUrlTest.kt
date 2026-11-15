package io.github.jqssun.airplay

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The push page is the only place a user is told why a link did not play, so the verdict has to
 * be right about the boring cases: pasted-with-spaces, upper case, credentials in the URL, and
 * anything that is not actually a stream.
 */
class StreamUrlTest {

    @Test
    fun `scheme is read case insensitively and after trimming`() {
        assertEquals("rtsp", StreamUrl.schemeOf("  RTSP://cam/live  "))
        assertEquals("https", StreamUrl.schemeOf("HTTPS://ex.com/a.mp4"))
        assertEquals("http", StreamUrl.schemeOf("http://ex.com/a.mp4"))
    }

    @Test
    fun `colon in a path or port is not a scheme`() {
        assertEquals("", StreamUrl.schemeOf("192.168.0.5:554/live"))
        assertEquals("", StreamUrl.schemeOf("/sdcard/a.mp4"))
        assertEquals("", StreamUrl.schemeOf("//ex.com/a.mp4"))
        assertEquals("", StreamUrl.schemeOf(""))
    }

    @Test
    fun `rtsp is recognised and http is not`() {
        assertTrue(StreamUrl.isRtsp("rtsp://192.168.0.9:554/Streaming/Channels/101"))
        assertFalse(StreamUrl.isRtsp("https://ex.com/a.mp4"))
    }

    @Test
    fun `playable urls pass`() {
        assertNull(StreamUrl.rejectReason("https://ex.com/a.mp4"))
        assertNull(StreamUrl.rejectReason("http://ex.com/live.m3u8"))
        assertNull(StreamUrl.rejectReason("rtsp://cam:554/live/ch1"))
        // user:pass@ is how IP cameras publish their credentials, and media3 does digest/basic
        assertNull(StreamUrl.rejectReason("rtsp://admin:secret@192.168.0.9:554/h264"))
    }

    @Test
    fun `rtsps is refused with its own message`() {
        val reason = StreamUrl.rejectReason("rtsps://cam:322/live")
        assertTrue(reason!!.contains("rtsps"))
        assertTrue(reason.contains("rtsp://"))
    }

    @Test
    fun `other schemes are refused`() {
        assertEquals("缺少链接", StreamUrl.rejectReason("   "))
        assertTrue(StreamUrl.rejectReason("file:///sdcard/a.mp4")!!.contains("rtsp"))
        assertTrue(StreamUrl.rejectReason("192.168.0.5:554/live")!!.contains("http://"))
    }

    @Test
    fun `truncated urls are refused before they reach the player`() {
        assertTrue(StreamUrl.rejectReason("rtsp://")!!.contains("地址"))
        assertTrue(StreamUrl.rejectReason("http:/ex.com/a.mp4")!!.contains("//"))
        assertTrue(StreamUrl.rejectReason("rtsp://:554/live")!!.contains("地址"))
    }

    @Test
    fun `live protocols are recognised by kind`() {
        assertEquals(StreamUrl.Kind.RTSP, StreamUrl.kindOf("rtsp://cam:554/1"))
        assertEquals(StreamUrl.Kind.RTMP, StreamUrl.kindOf("rtmp://host/live/a"))
        assertEquals(StreamUrl.Kind.RTMP, StreamUrl.kindOf("rtmps://host/live/a"))
        assertEquals(StreamUrl.Kind.UDP, StreamUrl.kindOf("udp://@239.1.1.1:1234"))
        assertEquals(StreamUrl.Kind.RTP, StreamUrl.kindOf("rtp://192.168.0.9:5006"))
        assertEquals(StreamUrl.Kind.HTTP, StreamUrl.kindOf("https://ex.com/a.mp4"))
        assertEquals(StreamUrl.Kind.OTHER, StreamUrl.kindOf("file:///a.mp4"))
        assertTrue(StreamUrl.isLiveStream("rtmp://host/live/a"))
        assertTrue(!StreamUrl.isLiveStream("https://ex.com/a.mp4"))
    }

    @Test
    fun `rtmp and datagram urls pass, but only when complete`() {
        assertNull(StreamUrl.rejectReason("rtmp://192.168.0.9:1935/live/cam1"))
        assertNull(StreamUrl.rejectReason("rtmps://host/live/cam1"))
        assertNull(StreamUrl.rejectReason("udp://@239.1.1.1:1234"))
        assertNull(StreamUrl.rejectReason("rtp://192.168.0.9:5006"))
        // portless datagram url: nothing to bind to
        assertTrue(StreamUrl.rejectReason("udp://239.1.1.1")!!.contains("端口"))
        // app present, stream name missing
        assertTrue(StreamUrl.rejectReason("rtmp://host/live")!!.contains("串流名"))
    }
}
