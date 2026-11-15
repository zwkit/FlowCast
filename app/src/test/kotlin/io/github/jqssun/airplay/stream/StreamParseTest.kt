package io.github.jqssun.airplay.stream

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The two URL shapes the player cannot rely on android.net.Uri for: RTMP splits into app + stream
 * name (both needed to build the connect command) and datagram URLs are written the ffmpeg way
 * (`udp://@239.1.1.1:1234`), where the host may be missing or prefixed with @.
 */
class StreamParseTest {

    @Test
    fun `rtmp url splits into app and stream name`() {
        val ep = RtmpUrl.parse("rtmp://192.168.0.9:1935/live/cam1")!!
        assertEquals("192.168.0.9", ep.host)
        assertEquals(1935, ep.port)
        assertEquals("live", ep.app)
        assertEquals("cam1", ep.streamName)
        assertEquals("rtmp://192.168.0.9:1935/live", ep.tcUrl)
        assertEquals(false, ep.tls)
    }

    @Test
    fun `rtmp url defaults its port and keeps a nested stream name`() {
        assertEquals(1935, RtmpUrl.parse("rtmp://host/live/a")!!.port)
        assertEquals(443, RtmpUrl.parse("rtmps://host/live/a")!!.port)
        assertEquals(true, RtmpUrl.parse("rtmps://host/live/a")!!.tls)
        assertEquals("a/b/c", RtmpUrl.parse("rtmp://h/app/a/b/c")!!.streamName)
    }

    @Test
    fun `rtmp url without a stream or host is rejected`() {
        // "live" alone is the app; there is nothing to play
        assertNull(RtmpUrl.parse("rtmp://host/live"))
        assertNull(RtmpUrl.parse("rtmp://:1935/live/a"))
        assertNull(RtmpUrl.parse("http://host/live/a"))
    }

    @Test
    fun `udp url accepts the ffmpeg spellings`() {
        val uni = UdpUrl.parse("udp://192.168.0.9:5004")!!
        assertEquals("192.168.0.9", uni.host)
        assertEquals(5004, uni.port)
        assertEquals(false, uni.multicast)

        // @-prefixed group with no local address: the usual IPTV form
        val multi = UdpUrl.parse("udp://@239.1.1.1:1234")!!
        assertEquals("239.1.1.1", multi.host)
        assertEquals(1234, multi.port)
        assertEquals(true, multi.multicast)

        // no host at all: receive from any source on that port
        val wildcard = UdpUrl.parse("udp://:5004")!!
        assertNull(wildcard.host)
        assertEquals(5004, wildcard.port)
    }

    @Test
    fun `udp url needs a real port`() {
        assertNull(UdpUrl.parse("udp://192.168.0.9"))
        assertNull(UdpUrl.parse("udp://192.168.0.9:0"))
        assertNull(UdpUrl.parse("udp://192.168.0.9:99999"))
        assertNull(UdpUrl.parse("rtp://"))
    }

    @Test
    fun `rtp url parses the same way`() {
        val t = UdpUrl.parse("rtp://192.168.0.9:5006")!!
        assertEquals("192.168.0.9", t.host)
        assertEquals(5006, t.port)
    }

    @Test
    fun `multicast detection covers ipv4 and ipv6`() {
        assertTrue(UdpUrl.isMulticastAddress("239.1.1.1"))
        assertTrue(UdpUrl.isMulticastAddress("224.0.0.1"))
        assertEquals(false, UdpUrl.isMulticastAddress("192.168.0.9"))
        assertTrue(UdpUrl.isMulticastAddress("ff02::1"))
    }
}
