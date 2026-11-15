package io.github.jqssun.airplay.stream

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.EOFException
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Chunk reassembly is where a hand-rolled RTMP client breaks first: one message is usually split
 * over several chunks, later chunks repeat only part of the header, and control messages are
 * interleaved with the media the player wants.
 */
class RtmpChunkTest {

    private fun clientFor(vararg chunks: ByteArray): RtmpClient {
        val input = ByteArrayInputStream(chunks.fold(ByteArray(0)) { a, b -> a + b })
        val endpoint = RtmpUrl.parse("rtmp://host/app/stream")!!
        return RtmpClient(endpoint, rtmpTransportOf(input, ByteArrayOutputStream()))
    }

    private fun u24(v: Int): ByteArray =
        byteArrayOf((v shr 16).toByte(), (v shr 8).toByte(), v.toByte())

    private fun u32(v: Long): ByteArray = byteArrayOf(
        (v shr 24).toByte(), (v shr 16).toByte(), (v shr 8).toByte(), v.toByte(),
    )

    private fun u32le(v: Int): ByteArray = byteArrayOf(
        v.toByte(), (v shr 8).toByte(), (v shr 16).toByte(), (v shr 24).toByte(),
    )

    private fun payload(n: Int, seed: Int = 0): ByteArray =
        ByteArray(n) { (it + seed).toByte() }

    @Test
    fun `a message spread over two chunks comes back whole`() {
        val data = payload(200)
        val first = byteArrayOf(0x06) + u24(1000) + u24(200) + byteArrayOf(9) + u32le(1) +
            data.copyOfRange(0, 128)
        // fmt 3: no headers at all, same chunk stream
        val second = byteArrayOf((3 shl 6 or 6).toByte()) + data.copyOfRange(128, 200)

        val msg = clientFor(first, second).readMessage()
        assertEquals(RtmpClient.TYPE_VIDEO, msg.type)
        assertEquals(1000L, msg.timestamp)
        assertEquals(200, msg.payload.size)
        assertTrue(msg.payload.contentEquals(data))
    }

    @Test
    fun `fmt 1 and 2 add their delta to the running timestamp`() {
        val base = byteArrayOf(0x06) + u24(500) + u24(4) + byteArrayOf(8) + u32le(1) + payload(4)
        // fmt 2 on the same stream: only a 3-byte delta, type/length repeat
        val delta = byteArrayOf((2 shl 6 or 6).toByte()) + u24(7) + payload(4, seed = 10)
        // fmt 1: delta + length + type
        val next = byteArrayOf((1 shl 6 or 6).toByte()) + u24(3) + u24(4) + byteArrayOf(9) +
            payload(4, seed = 20)

        val c = clientFor(base, delta, next)
        assertEquals(500L, c.readMessage().timestamp)
        assertEquals(507L, c.readMessage().timestamp)
        val third = c.readMessage()
        assertEquals(510L, third.timestamp)
        assertEquals(RtmpClient.TYPE_VIDEO, third.type)
    }

    @Test
    fun `extended timestamp replaces the 0xffffff marker`() {
        val big = 0x00012A05L // 76293 ms, above what three bytes hold
        val chunk = byteArrayOf(0x06) + u24(0xFFFFFF) + u24(2) + byteArrayOf(9) + u32le(1) +
            u32(big) + payload(2)
        val msg = clientFor(chunk).readMessage()
        assertEquals(big, msg.timestamp)
    }

    @Test
    fun `control messages are swallowed and set the incoming chunk size`() {
        // type 1 = Set Chunk Size 256, then a 200 byte message that now fits in a single chunk
        val control = byteArrayOf(0x02) + u24(0) + u24(4) + byteArrayOf(1) + u32le(0) + u32(256)
        val data = payload(200)
        val media = byteArrayOf(0x06) + u24(0) + u24(200) + byteArrayOf(9) + u32le(1) + data

        val msg = clientFor(control, media).readMessage()
        assertEquals(RtmpClient.TYPE_VIDEO, msg.type)
        assertTrue(msg.payload.contentEquals(data))
    }

    @Test
    fun `a truncated stream ends with an EOF rather than a silent empty message`() {
        val cut = byteArrayOf(0x06) + u24(0) + u24(200) + byteArrayOf(9) + u32le(1) + payload(10)
        val c = clientFor(cut)
        var threw = false
        try {
            c.readMessage()
        } catch (e: EOFException) {
            threw = true
        }
        assertTrue("incomplete chunk must not be reported as a complete message", threw)
    }
}
