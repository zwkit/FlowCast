package io.github.jqssun.airplay.audio

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** Unit tests for [DmapParser] (pure JVM, no Android dependencies). */
class DmapParserTest {

    private fun tag(tag: String, payload: ByteArray): ByteArray {
        val out = ByteArray(8 + payload.size)
        tag.toByteArray(Charsets.US_ASCII).copyInto(out, 0)
        // 4-byte big-endian length
        val len = payload.size
        out[4] = (len ushr 24).toByte()
        out[5] = (len ushr 16).toByte()
        out[6] = (len ushr 8).toByte()
        out[7] = len.toByte()
        payload.copyInto(out, 8)
        return out
    }

    private fun tag(tag: String, text: String) = tag(tag, text.toByteArray(Charsets.UTF_8))

    /** big-endian two's-complement encoding of [v] in [size] bytes (DMAP int payloads) */
    private fun beBytes(v: Long, size: Int): ByteArray {
        val out = ByteArray(size)
        for (i in 0 until size) out[i] = (v ushr (8 * (size - 1 - i))).toByte()
        return out
    }

    @Test
    fun `parses string tags`() {
        val data = tag("minm", "Song Title") + tag("asar", "Artist Name")
        val map = DmapParser.parse(data)
        assertEquals("Song Title", map["minm"])
        assertEquals("Artist Name", map["asar"])
    }

    @Test
    fun `parses int tags as big-endian`() {
        // astm = duration ms: 8-byte big-endian 215000
        val data = tag("astm", beBytes(215000L, 8))
        val map = DmapParser.parse(data)
        assertEquals(215000L, map["astm"])
    }

    @Test
    fun `parses short int tags`() {
        val data = tag("astn", byteArrayOf(0x00, 0x07.toByte())) // track 7, 2-byte
        val map = DmapParser.parse(data)
        assertEquals(7L, map["astn"])
    }

    @Test
    fun `parses nested containers`() {
        val inner = tag("minm", "Nested") + tag("astn", byteArrayOf(0, 3))
        val data = tag("mlit", inner)
        @Suppress("UNCHECKED_CAST")
        val container = DmapParser.parse(data)["mlit"] as Map<String, Any>
        assertEquals("Nested", container["minm"])
        assertEquals(3L, container["astn"])
    }

    @Test
    fun `stops at truncated payload`() {
        // header claims 100 bytes but only 2 follow -> must not throw, stops cleanly
        val data = tag("minm", "ab") + byteArrayOf('m'.code.toByte(), 'i'.code.toByte(),
            'n'.code.toByte(), 'm'.code.toByte(), 0, 0, 0, 100, 1, 2)
        val map = DmapParser.parse(data)
        assertEquals("ab", map["minm"])
        assertTrue(map.keys == setOf("minm"))
    }

    @Test
    fun `empty input yields empty map`() {
        assertTrue(DmapParser.parse(ByteArray(0)).isEmpty())
        assertTrue(DmapParser.parse(ByteArray(4)).isEmpty()) // too short for a header
    }

    @Test
    fun `parses multiple mixed items in order`() {
        // miid is an int tag; mikd is not in INT_TAGS and parses as a string
        val data = tag("miid", beBytes(1L, 4)) +
            tag("minm", "Title") +
            tag("asdk", beBytes(1L, 4)) +
            tag("mikd", byteArrayOf(1)) +
            tag("mlit", tag("minm", "Child"))
        val map = DmapParser.parse(data)
        assertEquals(5, map.size)
        assertEquals(1L, map["miid"])
        assertEquals("Title", map["minm"])
        assertEquals(1L, map["asdk"])
        assertEquals("\u0001", map["mikd"])
        assertTrue(map["mlit"] is Map<*, *>)
    }
}
