package io.github.jqssun.airplay.stream

import android.content.Context
import android.net.Uri
import android.net.wifi.WifiManager
import androidx.media3.common.C
import androidx.media3.common.util.UnstableApi
import androidx.media3.datasource.BaseDataSource
import androidx.media3.datasource.DataSource
import androidx.media3.datasource.DataSpec
import java.io.IOException
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.MulticastSocket

/**
 * MPEG-TS arriving as datagrams: `udp://` straight from the wire, `rtp://` with the RTP wrapper
 * taken off first.
 *
 * Written instead of media3's own UdpDataSource for two reasons: it has to cope with the ffmpeg
 * spelling of multicast (`udp://@239.1.1.1:1234`, where the host is empty or prefixed with @),
 * and the RTP variant needs the 12+ byte header removed per datagram, which can only be done on
 * packet boundaries — UdpDataSource hands out arbitrary slices of a packet and would split the
 * header across reads.
 *
 * The payload is demuxed as MPEG-TS (payload type 33 in the RTP case, the usual IPTV/encoder
 * mode). Anything else — bare H.264 over RTP, for instance — needs an SDP to be decodable at all
 * and is reported as a playback error rather than guessed at.
 */
@UnstableApi
class UdpStreamDataSource(
    private val stripRtp: Boolean,
    private val appContext: Context? = null,
) : BaseDataSource(/* isNetwork= */ true) {

    class Factory(
        private val stripRtp: Boolean,
        private val appContext: Context? = null,
    ) : DataSource.Factory {
        override fun createDataSource(): DataSource = UdpStreamDataSource(stripRtp, appContext)
    }

    companion object {
        const val SOCKET_TIMEOUT_MS = 15000
        private const val MAX_DATAGRAM = 65535
    }

    private var socket: DatagramSocket? = null
    private var multicastLock: WifiManager.MulticastLock? = null
    private val packetBuf = ByteArray(MAX_DATAGRAM)
    private var pending: ByteArray? = null
    private var pendingOffset = 0
    private var uri: Uri? = null

    override fun open(dataSpec: DataSpec): Long {
        val u = dataSpec.uri
        uri = u
        transferInitializing(dataSpec)
        val target = UdpUrl.parse(u.toString())
            ?: throw IOException("udp: 地址或端口无效（$u）")
        val sock: DatagramSocket = try {
            if (target.multicast && target.host != null) {
                val ms = MulticastSocket(target.port)
                ms.joinGroup(InetAddress.getByName(target.host))
                acquireMulticastLock()
                ms
            } else if (target.host != null) {
                DatagramSocket(InetSocketAddress(InetAddress.getByName(target.host), target.port))
            } else {
                DatagramSocket(target.port)
            }
        } catch (t: Throwable) {
            throw IOException("udp: 无法接收 ${target.host ?: "0.0.0.0"}:${target.port}", t)
        }
        sock.soTimeout = SOCKET_TIMEOUT_MS
        socket = sock
        transferStarted(dataSpec)
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
            val sock = socket ?: throw IOException("udp source closed")
            val packet = DatagramPacket(packetBuf, MAX_DATAGRAM)
            sock.receive(packet)
            val payload = payloadOf(packet)
            if (payload.isEmpty()) continue
            pending = payload
            pendingOffset = 0
        }
    }

    override fun getUri(): Uri? = uri

    override fun close() {
        pending = null
        try {
            socket?.close()
        } catch (t: Throwable) {
            // closing a datagram socket is best effort
        } finally {
            socket = null
            releaseMulticastLock()
            transferEnded()
        }
    }

    /** Whole datagram for udp://, RTP payload for rtp://. */
    private fun payloadOf(packet: DatagramPacket): ByteArray {
        val len = packet.length
        if (!stripRtp || len < 12) {
            return packet.data.copyOfRange(packet.offset, packet.offset + len)
        }
        val d = packet.data
        val o = packet.offset
        val b0 = d[o].toInt() and 0xFF
        val version = b0 shr 6
        if (version != 2) {
            // not an RTP packet at all — hand it over untouched so plain TS still works
            return d.copyOfRange(o, o + len)
        }
        val padding = (b0 shr 5) and 1
        val extension = (b0 shr 4) and 1
        val csrcCount = b0 and 0x0F
        var headerLen = 12 + 4 * csrcCount
        if (extension == 1 && len >= headerLen + 4) {
            val extWords = ((d[o + headerLen + 2].toInt() and 0xFF) shl 8) or
                (d[o + headerLen + 3].toInt() and 0xFF)
            headerLen += 4 + 4 * extWords
        }
        if (headerLen >= len) return ByteArray(0)
        var end = len
        if (padding == 1 && len > headerLen) {
            end -= d[o + len - 1].toInt() and 0xFF
        }
        if (end <= headerLen) return ByteArray(0)
        return d.copyOfRange(o + headerLen, o + end)
    }

    /**
     * Android drops multicast unless somebody holds a lock while the socket is joined; harmless
     * on Ethernet, where no lock is needed but the call still succeeds.
     */
    private fun acquireMulticastLock() {
        val ctx = appContext ?: return
        runCatching {
            val wifi = ctx.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
            val lock = wifi?.createMulticastLock("flowcast-udp")
            lock?.setReferenceCounted(false)
            lock?.acquire()
            multicastLock = lock
        }
    }

    private fun releaseMulticastLock() {
        runCatching { multicastLock?.release() }
        multicastLock = null
    }
}
