package io.github.jqssun.airplay.stream

import java.io.ByteArrayOutputStream
import java.io.Closeable
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.charset.StandardCharsets
import javax.net.ssl.SSLSocketFactory

/**
 * RTMP playback ("play" side) in plain Kotlin, because media3 has no RTMP at all: Google dropped
 * the librtmp-backed ExoPlayer extension and never ported it, so `rtmp://` is the one live
 * protocol a TV receiver still meets daily (OBS / nginx-rtmp / SRS / MediaMTX) that ExoPlayer
 * cannot open.
 *
 * What comes back from the server is *not* a container. RTMP carries the very same tag payloads
 * an FLV file stores, so the client hands out [RtmpMessage]s and [RtmpDataSource] re-wraps them
 * into an FLV byte stream that media3's own FlvExtractor demuxes. That is why no codec work is
 * needed here: H.264/AVC and AAC config already arrive as ordinary video/audio messages.
 *
 * Deliberately Android-free (java.net + java.io only) so the chunk/AMF0 machinery can be unit
 * tested off-device; [RtmpTransport] is the seam a test feeds bytes through.
 */

class RtmpMessage(val type: Int, val timestamp: Long, val payload: ByteArray)

/** rtmp://host[:port]/app/streamName, split into the pieces a connect needs. */
data class RtmpEndpoint(
    val scheme: String,
    val host: String,
    val port: Int,
    /** first path segment — the application the server publishes under */
    val app: String,
    /** everything after the app — the stream to play */
    val streamName: String,
) {
    val tls: Boolean get() = scheme == "rtmps"
    val tcUrl: String get() = "$scheme://$host:$port/$app"
}

object RtmpUrl {
    private const val DEFAULT_PORT = 1935
    private const val DEFAULT_TLS_PORT = 443

    /** Null when [raw] is not an RTMP URL a connect can be built from. */
    fun parse(raw: String): RtmpEndpoint? {
        val s = raw.trim()
        val scheme = s.substringBefore("://", "").lowercase()
        if (scheme != "rtmp" && scheme != "rtmps") return null
        if (s.indexOf("://") < 0) return null
        val rest = s.substringAfter("://").substringBefore('?').substringBefore('#')
        val authority = rest.substringBefore('/')
        val path = rest.substringAfter('/', "")
        // "rtmp://user:pass@host/app/stream" — credentials are not used, RTMP has no auth here
        val host = authority.substringAfterLast('@').substringBefore(':')
        if (host.isBlank()) return null
        val port = authority.substringAfter(':', "").toIntOrNull()
            ?: if (scheme == "rtmps") DEFAULT_TLS_PORT else DEFAULT_PORT
        val app = path.substringBefore('/').trim()
        if (app.isEmpty()) return null
        // "rtmp://host/live" names an application, not something to play
        val streamName = path.substringAfter('/', "").trim()
        if (streamName.isEmpty()) return null
        return RtmpEndpoint(scheme, host, port, app, streamName)
    }
}

/** The socket pair a session reads and writes. Tests substitute both ends. */
internal class RtmpTransport(
    val input: DataInputStream,
    val output: DataOutputStream,
    private val onClose: () -> Unit,
) : Closeable {
    override fun close() = onClose()
}

class RtmpClient internal constructor(
    private val endpoint: RtmpEndpoint,
    private val transportOverride: RtmpTransport? = null,
) : Closeable {

    companion object {
        const val CONNECT_TIMEOUT_MS = 8000
        const val READ_TIMEOUT_MS = 15000
        const val HANDSHAKE_SIZE = 1536
        const val DEFAULT_CHUNK_SIZE = 128

        // RTMP message types
        const val TYPE_SET_CHUNK_SIZE = 1
        const val TYPE_ABORT = 2
        const val TYPE_ACK = 3
        const val TYPE_USER_CONTROL = 4
        const val TYPE_WINDOW_ACK_SIZE = 5
        const val TYPE_PEER_BANDWIDTH = 6
        const val TYPE_AUDIO = 8
        const val TYPE_VIDEO = 9
        const val TYPE_DATA_AMF3 = 15
        const val TYPE_COMMAND_AMF3 = 17
        const val TYPE_DATA_AMF0 = 18
        const val TYPE_COMMAND_AMF0 = 20

        // user control events
        private const val EVENT_PING_REQUEST = 6
        private const val EVENT_PING_RESPONSE = 7

        private const val CSID_COMMAND = 3
    }

    private var socket: Socket? = null
    // tests inject the transport; a real session creates it in open()
    private var transport: RtmpTransport? = transportOverride
    private val input: DataInputStream get() = transport!!.input
    private val output: DataOutputStream get() = transport!!.output

    private val streams = HashMap<Int, ChunkStream>()
    private var inChunkSize = DEFAULT_CHUNK_SIZE
    private var bytesRead = 0L
    private var playStreamId = 0

    private class ChunkStream {
        var length = 0
        var type = 0
        var timestamp = 0L
        var streamId = 0
        var buffer: ByteArray? = null
        var filled = 0
    }

    /** Handshake + connect + createStream + play. Blocks; throws on any protocol refusal. */
    @Throws(IOException::class)
    fun open() {
        val own = transportOverride
        if (own != null) {
            transport = own
        } else {
            val plain = Socket()
            plain.connect(InetSocketAddress(endpoint.host, endpoint.port), CONNECT_TIMEOUT_MS)
            plain.soTimeout = READ_TIMEOUT_MS
            // rtmps is RTMP over TLS on 443; nothing else about the protocol changes.
            // getDefault() is declared to return SocketFactory, which has no layering overload,
            // hence the cast back to the SSL type.
            val s = if (endpoint.tls) {
                val factory = SSLSocketFactory.getDefault() as SSLSocketFactory
                factory.createSocket(plain, endpoint.host, endpoint.port, true)
                    .also { it.soTimeout = READ_TIMEOUT_MS }
            } else {
                plain
            }
            socket = s
            transport = RtmpTransport(
                DataInputStream(s.getInputStream()),
                DataOutputStream(s.getOutputStream()),
            ) { runCatching { s.close() } }
        }
        try {
            handshake()
            sendConnect()
            awaitResult(1.0)
            playStreamId = requestCreateStream()
            sendPlay()
        } catch (t: Throwable) {
            close()
            throw t
        }
    }

    /** Next media/notify message; control messages are consumed on the way. */
    @Throws(IOException::class)
    fun readMessage(): RtmpMessage {
        while (true) {
            val msg = readChunkedMessage() ?: continue
            if (handleControl(msg)) continue
            return msg
        }
    }

    override fun close() {
        streams.clear()
        transport?.close()
        transport = null
        socket = null
    }

    // ---------------------------------------------------------------- handshake

    @Throws(IOException::class)
    private fun handshake() {
        // C0+C1: version 3, then 1536 bytes of (time, zeroes, random). Servers pick the simple
        // handshake because the "zeroes" field is all-zero; the digest variant needs HMAC-SHA256.
        output.writeByte(3)
        output.write(ByteArray(HANDSHAKE_SIZE))
        output.flush()

        val s0 = input.readUnsignedByte()
        if (s0 != 3) throw IOException("rtmp: server handshake version $s0")
        val s1 = ByteArray(HANDSHAKE_SIZE)
        input.readFully(s1)
        // C2 echoes S1 verbatim; servers that check it want both timestamps back
        output.write(s1)
        output.flush()
        val s2 = ByteArray(HANDSHAKE_SIZE)
        input.readFully(s2)
        bytesRead += 1 + HANDSHAKE_SIZE * 2
    }

    // ---------------------------------------------------------------- commands

    @Throws(IOException::class)
    private fun sendConnect() {
        val amf = Amf0Writer()
        amf.string("connect")
        amf.number(1.0)
        amf.objectStart()
        amf.property("app", endpoint.app)
        amf.property("flashVer", "FMLE/3.0 (compatible; FlowCast)")
        amf.property("tcUrl", endpoint.tcUrl)
        amf.property("fpad", false)
        amf.property("capabilities", 15.0)
        amf.property("audioCodecs", 4071.0) // AAC + MP3 + everything FMLE advertises
        amf.property("videoCodecs", 252.0) // H.264 (AVC)
        amf.property("videoFunction", 1.0)
        amf.property("objectEncoding", 0.0)
        amf.objectEnd()
        send(CSID_COMMAND, TYPE_COMMAND_AMF0, 0, amf.toByteArray())
    }

    @Throws(IOException::class)
    private fun requestCreateStream(): Int {
        val amf = Amf0Writer()
        amf.string("createStream")
        amf.number(2.0)
        amf.nil()
        send(CSID_COMMAND, TYPE_COMMAND_AMF0, 0, amf.toByteArray())
        val result = awaitResult(2.0)
        return (result.fourth as? Double)?.toInt() ?: 1
    }

    @Throws(IOException::class)
    private fun sendPlay() {
        val amf = Amf0Writer()
        amf.string("play")
        amf.number(0.0)
        amf.nil()
        amf.string(endpoint.streamName)
        // -2 = live only. Asking for a recording that does not exist makes some servers stall
        // instead of falling back, and a pushed link is always meant to be live.
        amf.number(-2.0)
        send(CSID_COMMAND, TYPE_COMMAND_AMF0, playStreamId, amf.toByteArray())
    }

    /** Reads past onStatus/notify noise until the server answers transaction [transId]. */
    @Throws(IOException::class)
    private fun awaitResult(transId: Double): RtmpCommand {
        while (true) {
            val msg = readChunkedMessage() ?: continue
            if (handleControl(msg)) continue
            if (msg.type != TYPE_COMMAND_AMF0 && msg.type != TYPE_COMMAND_AMF3) continue
            val cmd = RtmpCommand.decode(msg.payload) ?: continue
            if (cmd.transactionId != transId) continue
            if (cmd.isError) {
                throw IOException("rtmp ${cmd.name}: ${cmd.description ?: "server refused"}")
            }
            if (cmd.name == "_result") return cmd
        }
    }

    // ---------------------------------------------------------------- chunking

    /** One chunk, or null while the current message is still spread over more chunks. */
    @Throws(IOException::class)
    private fun readChunkedMessage(): RtmpMessage? {
        val b0 = input.readUnsignedByte().also { bytesRead++ }
        val fmt = b0 shr 6
        var csId = b0 and 0x3F
        if (csId == 0) {
            csId = 64 + input.readUnsignedByte()
            bytesRead++
        } else if (csId == 1) {
            csId = 64 + input.readUnsignedByte() + (input.readUnsignedByte() shl 8)
            bytesRead += 2
        }
        val cs = streams.getOrPut(csId) { ChunkStream() }

        // fmt 3 repeats the previous header, so "no timestamp field" has to read as a zero delta
        var tsField = 0
        when (fmt) {
            0 -> {
                tsField = readU24()
                cs.length = readU24()
                cs.type = input.readUnsignedByte().also { bytesRead++ }
                cs.streamId = readU32Le()
            }
            1 -> {
                tsField = readU24()
                cs.length = readU24()
                cs.type = input.readUnsignedByte().also { bytesRead++ }
            }
            2 -> {
                tsField = readU24()
            }
            3 -> { /* everything repeats from the previous chunk of this stream */ }
            else -> { /* fmt is 2 bits wide, so 0..3 is exhaustive */ }
        }
        // 0xFFFFFF is a marker: the real value follows as a 4-byte extended timestamp. fmt 3 has
        // none, it simply keeps using the previous one.
        val delta: Long = if (tsField == 0xFFFFFF) readU32() else tsField.toLong()
        if (fmt == 0) {
            cs.timestamp = delta
        } else {
            cs.timestamp += delta
        }

        if (cs.buffer == null && cs.length > 0) cs.buffer = ByteArray(cs.length)
        val want = minOf(cs.length - cs.filled, inChunkSize)
        if (want > 0) {
            input.readFully(cs.buffer!!, cs.filled, want)
            cs.filled += want
            bytesRead += want
        }
        if (cs.filled < cs.length) {
            // message spans more chunks: keep going with whatever headers the next chunk carries
            return null
        }
        val payload = cs.buffer ?: ByteArray(0)
        val msg = RtmpMessage(cs.type, cs.timestamp, payload)
        cs.buffer = null
        cs.filled = 0
        return msg
    }

    @Throws(IOException::class)
    private fun readU24(): Int {
        val a = input.readUnsignedByte()
        val b = input.readUnsignedByte()
        val c = input.readUnsignedByte()
        bytesRead += 3
        return (a shl 16) or (b shl 8) or c
    }

    @Throws(IOException::class)
    private fun readU32(): Long {
        val a = input.readUnsignedByte().toLong()
        val b = input.readUnsignedByte().toLong()
        val c = input.readUnsignedByte().toLong()
        val d = input.readUnsignedByte().toLong()
        bytesRead += 4
        return (a shl 24) or (b shl 16) or (c shl 8) or d
    }

    /** Message stream id is the one little-endian field in the chunk header. */
    @Throws(IOException::class)
    private fun readU32Le(): Int {
        val a = input.readUnsignedByte()
        val b = input.readUnsignedByte()
        val c = input.readUnsignedByte()
        val d = input.readUnsignedByte()
        bytesRead += 4
        return a or (b shl 8) or (c shl 16) or (d shl 24)
    }

    @Throws(IOException::class)
    private fun send(csId: Int, type: Int, streamId: Int, payload: ByteArray, timestamp: Int = 0) {
        output.writeByte((0 shl 6) or csId)
        writeU24(timestamp)
        writeU24(payload.size)
        output.writeByte(type)
        writeU32Le(streamId)
        var off = 0
        while (off < payload.size) {
            val n = minOf(payload.size - off, DEFAULT_CHUNK_SIZE)
            output.write(payload, off, n)
            off += n
            // continuation chunks: fmt 3 on the same stream, no headers repeated
            if (off < payload.size) output.writeByte((3 shl 6) or csId)
        }
        output.flush()
    }

    @Throws(IOException::class)
    private fun writeU24(value: Int) {
        output.writeByte((value shr 16) and 0xFF)
        output.writeByte((value shr 8) and 0xFF)
        output.writeByte(value and 0xFF)
    }

    @Throws(IOException::class)
    private fun writeU32Le(value: Int) {
        output.writeByte(value and 0xFF)
        output.writeByte((value shr 8) and 0xFF)
        output.writeByte((value shr 16) and 0xFF)
        output.writeByte((value shr 24) and 0xFF)
    }

    // ---------------------------------------------------------------- control

    @Throws(IOException::class)
    private fun handleControl(msg: RtmpMessage): Boolean {
        when (msg.type) {
            TYPE_SET_CHUNK_SIZE -> {
                if (msg.payload.size >= 4) {
                    val v = u32be(msg.payload, 0).toInt()
                    // top bit is reserved; servers that send 0 mean "use default"
                    inChunkSize = if (v <= 0) DEFAULT_CHUNK_SIZE else (v and 0x7FFFFFFF)
                }
            }
            TYPE_WINDOW_ACK_SIZE -> sendAck()
            TYPE_USER_CONTROL -> {
                if (msg.payload.size >= 6 && u16be(msg.payload, 0) == EVENT_PING_REQUEST) {
                    val ping = msg.payload.copyOfRange(2, 6)
                    val body = ByteArray(6)
                    body[0] = (EVENT_PING_RESPONSE shr 8).toByte()
                    body[1] = (EVENT_PING_RESPONSE and 0xFF).toByte()
                    System.arraycopy(ping, 0, body, 2, 4)
                    send(2, TYPE_USER_CONTROL, 0, body)
                }
            }
            TYPE_ABORT, TYPE_ACK, TYPE_PEER_BANDWIDTH -> Unit
            else -> return false
        }
        return true
    }

    @Throws(IOException::class)
    private fun sendAck() {
        val body = ByteArray(4)
        val v = bytesRead
        body[0] = ((v shr 24) and 0xFF).toByte()
        body[1] = ((v shr 16) and 0xFF).toByte()
        body[2] = ((v shr 8) and 0xFF).toByte()
        body[3] = (v and 0xFF).toByte()
        send(2, TYPE_ACK, 0, body)
    }

    private fun u16be(b: ByteArray, o: Int): Int = ((b[o].toInt() and 0xFF) shl 8) or (b[o + 1].toInt() and 0xFF)

    private fun u32be(b: ByteArray, o: Int): Long =
        ((b[o].toLong() and 0xFF) shl 24) or ((b[o + 1].toLong() and 0xFF) shl 16) or
            ((b[o + 2].toLong() and 0xFF) shl 8) or (b[o + 3].toLong() and 0xFF)
}

// ---------------------------------------------------------------------------- AMF0

internal class Amf0Writer {
    private val out = ByteArrayOutputStream()

    fun number(v: Double) {
        out.write(0x00)
        val bits = java.lang.Double.doubleToRawLongBits(v)
        for (i in 7 downTo 0) out.write(((bits ushr (i * 8)) and 0xFF).toInt())
    }

    fun boolean(v: Boolean) {
        out.write(0x01)
        out.write(if (v) 1 else 0)
    }

    fun string(v: String) {
        val b = v.toByteArray(StandardCharsets.UTF_8)
        if (b.size > 0xFFFF) { // long string, only reachable with a pathological stream name
            out.write(0x0C)
            for (i in 3 downTo 0) out.write(((b.size ushr (i * 8)) and 0xFF).toInt())
        } else {
            out.write(0x02)
            out.write((b.size shr 8) and 0xFF)
            out.write(b.size and 0xFF)
        }
        out.write(b)
    }

    fun nil() = out.write(0x05)

    fun objectStart() = out.write(0x03)

    fun property(name: String, value: String) {
        key(name)
        string(value)
    }

    fun property(name: String, value: Double) {
        key(name)
        number(value)
    }

    fun property(name: String, value: Boolean) {
        key(name)
        boolean(value)
    }

    fun objectEnd() {
        out.write(0x00)
        out.write(0x00)
        out.write(0x09)
    }

    private fun key(name: String) {
        val b = name.toByteArray(StandardCharsets.UTF_8)
        out.write((b.size shr 8) and 0xFF)
        out.write(b.size and 0xFF)
        out.write(b)
    }

    fun toByteArray(): ByteArray = out.toByteArray()
}

internal class Amf0Reader(private val b: ByteArray) {
    private var pos = 0

    fun readValue(): Any? {
        if (pos >= b.size) return null
        val marker = b[pos++].toInt() and 0xFF
        return when (marker) {
            0x00 -> readDouble()
            0x01 -> readBoolean()
            0x02 -> readString(u16())
            0x03 -> readObject()
            0x05, 0x06 -> null
            0x08 -> readEcmaArray()
            0x0A -> readStrictArray()
            0x0B -> { // date: 8-byte millis + 2-byte timezone
                readDouble()
                skip(2)
                null
            }
            0x0C -> readString(u32().toInt())
            else -> null // 0x04/0x07/0x0D and friends carry nothing we act on
        }
    }

    private fun readDouble(): Double {
        if (pos + 8 > b.size) return Double.NaN
        var bits = 0L
        for (i in 0..7) bits = (bits shl 8) or (b[pos++].toLong() and 0xFF)
        return java.lang.Double.longBitsToDouble(bits)
    }

    private fun readBoolean(): Boolean {
        val v = b.getOrNull(pos)?.toInt()?.and(0xFF) ?: 0
        pos++
        return v != 0
    }

    private fun readString(len: Int): String {
        val n = minOf(len, b.size - pos).coerceAtLeast(0)
        val s = String(b, pos, n, StandardCharsets.UTF_8)
        pos += len
        return s
    }

    private fun readObject(): Map<String, Any?> {
        val map = LinkedHashMap<String, Any?>()
        while (pos + 2 < b.size) {
            val keyLen = u16()
            if (keyLen == 0) { // object end is 0x00 0x00 0x09
                pos++
                break
            }
            if (pos + keyLen > b.size) break
            val key = String(b, pos, keyLen, StandardCharsets.UTF_8)
            pos += keyLen
            if (pos >= b.size) break
            if ((b[pos].toInt() and 0xFF) == 0x09) { pos++; break }
            map[key] = readValue()
        }
        return map
    }

    /** Strict array: a count, then that many values. */
    private fun readStrictArray(): List<Any?> {
        val n = minOf(u32().toInt(), 1024)
        val list = ArrayList<Any?>(n)
        repeat(n) { list.add(readValue()) }
        return list
    }

    private fun readEcmaArray(): Map<String, Any?> {
        skip(4) // declared count; the object-end marker is what actually terminates
        return readObject()
    }

    private fun u16(): Int {
        if (pos + 2 > b.size) return 0
        val v = ((b[pos].toInt() and 0xFF) shl 8) or (b[pos + 1].toInt() and 0xFF)
        pos += 2
        return v
    }

    private fun u32(): Long {
        if (pos + 4 > b.size) return 0L
        var v = 0L
        for (i in 0..3) v = (v shl 8) or (b[pos++].toLong() and 0xFF)
        return v
    }

    private fun skip(n: Int) {
        pos = minOf(pos + n, b.size)
    }
}

/** A decoded AMF0 command: `_result`, `_error`, `onStatus`, `play`, ... */
internal class RtmpCommand(
    val name: String,
    val transactionId: Double,
    val third: Any?,
    val fourth: Any?,
) {
    val isError: Boolean get() = name == "_error"
    val description: String? get() {
        val info = fourth as? Map<*, *> ?: return null
        return info["description"] as? String
    }

    companion object {
        fun decode(payload: ByteArray): RtmpCommand? {
            val r = Amf0Reader(payload)
            val name = r.readValue() as? String ?: return null
            val trans = r.readValue() as? Double ?: return null
            return RtmpCommand(name, trans, r.readValue(), r.readValue())
        }
    }
}

/** Drain helper used by tests to build a transport out of byte arrays. */
internal fun rtmpTransportOf(input: InputStream, output: OutputStream, onClose: () -> Unit = {}) =
    RtmpTransport(DataInputStream(input), DataOutputStream(output), onClose)
