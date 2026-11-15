package io.github.jqssun.airplay.stream

import android.net.Uri
import androidx.media3.common.C
import androidx.media3.common.util.UnstableApi
import androidx.media3.datasource.BaseDataSource
import androidx.media3.datasource.DataSource
import androidx.media3.datasource.DataSpec
import java.io.ByteArrayOutputStream
import java.io.IOException

/**
 * Presents an RTMP session to ExoPlayer as if it were an endless FLV file.
 *
 * RTMP does not hand out a container — it hands out FLV tag payloads one message at a time. Rather
 * than demuxing them ourselves we prepend a synthetic FLV header and re-wrap every message into a
 * proper tag (header + payload + previous-tag-size), which is exactly what media3's FlvExtractor
 * already reads. H.264 sequence headers and AAC AudioSpecificConfig arrive as ordinary
 * video/audio messages, so the decoder gets configured without any extra work here.
 */
@UnstableApi
class RtmpDataSource : BaseDataSource(/* isNetwork= */ true) {

    class Factory : DataSource.Factory {
        override fun createDataSource(): DataSource = RtmpDataSource()
    }

    private var client: RtmpClient? = null
    private var uri: Uri? = null
    /** bytes of the FLV tag currently being handed out */
    private var pending: ByteArray? = null
    private var pendingOffset = 0

    override fun open(dataSpec: DataSpec): Long {
        val u = dataSpec.uri
        uri = u
        transferInitializing(dataSpec)
        val endpoint = RtmpUrl.parse(u.toString())
            ?: throw IOException("not an rtmp url: $u")
        val c = RtmpClient(endpoint)
        try {
            c.open()
        } catch (t: Throwable) {
            runCatching { c.close() }
            throw t
        }
        client = c
        // FLV header (9 bytes) + the leading 4-byte "previous tag size" of 0
        pending = byteArrayOf(
            0x46, 0x4C, 0x56, 0x01, 0x05, 0x00, 0x00, 0x00, 0x09,
            0x00, 0x00, 0x00, 0x00,
        )
        pendingOffset = 0
        transferStarted(dataSpec)
        // a live stream has no end: reporting a length would make ExoPlayer wait for an EOF that
        // never comes
        return C.LENGTH_UNSET.toLong()
    }

    override fun read(buffer: ByteArray, offset: Int, length: Int): Int {
        if (length == 0) return 0
        while (true) {
            val p = pending
            if (p != null && pendingOffset < p.size) {
                val n = minOf(length, p.size - pendingOffset)
                System.arraycopy(p, pendingOffset, buffer, offset, n)
                pendingOffset += n
                bytesTransferred(n)
                return n
            }
            pending = null
            // blocking: the stream keeps the player fed until it is stopped or the server drops
            val msg = client?.readMessage() ?: throw IOException("rtmp source closed")
            val tag = flvTagOf(msg) ?: continue
            pending = tag
            pendingOffset = 0
        }
    }

    override fun getUri(): Uri? = uri

    override fun close() {
        pending = null
        try {
            client?.close()
        } finally {
            client = null
            transferEnded()
        }
    }

    private fun flvTagOf(msg: RtmpMessage): ByteArray? {
        // 8/9 are audio/video; 18 is AMF0 script data (onMetaData). AMF3 metadata (15) would have
        // to be transcoded into AMF0 to be a valid FLV tag, so it is dropped rather than faked.
        val tagType = when (msg.type) {
            RtmpClient.TYPE_AUDIO -> 8
            RtmpClient.TYPE_VIDEO -> 9
            RtmpClient.TYPE_DATA_AMF0 -> 18
            else -> return null
        }
        val out = ByteArrayOutputStream(11 + msg.payload.size + 4)
        out.write(tagType)
        val size = msg.payload.size
        out.write((size shr 16) and 0xFF)
        out.write((size shr 8) and 0xFF)
        out.write(size and 0xFF)
        // FLV timestamps are 24 bits plus one extension byte above them — the same layout RTMP
        // uses, so the value needs no conversion
        val ts = msg.timestamp and 0xFFFFFFFFL
        out.write(((ts shr 16) and 0xFF).toInt())
        out.write(((ts shr 8) and 0xFF).toInt())
        out.write((ts and 0xFF).toInt())
        out.write(((ts shr 24) and 0xFF).toInt())
        out.write(0)
        out.write(0)
        out.write(0) // stream id, always 0 in FLV
        out.write(msg.payload)
        val total = 11 + size
        out.write(((total shr 24) and 0xFF).toInt())
        out.write(((total shr 16) and 0xFF).toInt())
        out.write(((total shr 8) and 0xFF).toInt())
        out.write(total and 0xFF)
        return out.toByteArray()
    }
}
