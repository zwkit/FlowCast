package io.github.jqssun.airplay.upnp

import android.content.Context
import android.net.wifi.WifiManager
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.MulticastSocket
import java.nio.charset.StandardCharsets
import java.util.concurrent.atomic.AtomicBoolean

/** SSDP responder/advertiser so phones discover this device as a UPnP MediaRenderer. */
internal class SsdpAdvertiser(
    private val context: Context,
    private val uuid: String,
    private val httpPort: Int,
) {
    private val group = "239.255.255.250"
    private val port = 1900
    private val socket: MulticastSocket? = runCatching {
        MulticastSocket(port).apply { reuseAddress = true; timeToLive = 4 }
    }.getOrNull()
    private val running = AtomicBoolean(false)
    private var listener: Thread? = null
    private var notifier: Thread? = null
    private var multicastLock: WifiManager.MulticastLock? = null

    private val deviceType = "urn:schemas-upnp-org:device:MediaRenderer:1"
    private val avtService = "urn:schemas-upnp-org:service:AVTransport:1"
    private val cmService = "urn:schemas-upnp-org:service:ConnectionManager:1"
    private val rcService = "urn:schemas-upnp-org:service:RenderingControl:1"

    fun start() {
        if (!running.getAndSet(true)) {
            (context.getSystemService(Context.WIFI_SERVICE) as? WifiManager)?.let { wm ->
                multicastLock = wm.createMulticastLock("dlna").apply { setReferenceCounted(false); acquire() }
            }
            socket?.let { s ->
                runCatching { s.joinGroup(InetAddress.getByName(group)) }
                listener = Thread({ listenLoop(s) }, "dlna-ssdp").also { it.isDaemon = true; it.start() }
                notifier = Thread({ notifyLoop() }, "dlna-ssdp-notify").also { it.isDaemon = true; it.start() }
            }
        }
    }

    fun stop() {
        running.set(false)
        runCatching { socket?.leaveGroup(InetAddress.getByName(group)) }
        runCatching { socket?.close() }
        multicastLock?.let { runCatching { it.release() } }
        multicastLock = null
        listener?.interrupt()
        notifier?.interrupt()
        listener = null
        notifier = null
    }

    private fun listenLoop(s: MulticastSocket) {
        val buf = ByteArray(4096)
        while (running.get()) {
            val dp = DatagramPacket(buf, buf.size)
            try { s.receive(dp) } catch (e: Exception) { if (running.get()) continue else return }
            val msg = String(dp.data, 0, dp.length, StandardCharsets.UTF_8)
            if (msg.startsWith("M-SEARCH")) handleMSearch(msg, dp.address, dp.port)
        }
    }

    private fun handleMSearch(msg: String, src: InetAddress, srcPort: Int) {
        val lines = msg.split("\r\n", "\n")
        val st = lines.firstOrNull { it.startsWith("ST:", true) }?.substringAfter(':')?.trim() ?: return
        // MX is sender-chosen; clamp to the UPnP range so a hostile/buggy control point
        // can't make us advertise a huge max-age
        val mx = (lines.firstOrNull { it.startsWith("MX:", true) }?.substringAfter(':')?.trim()?.toIntOrNull() ?: 1)
            .coerceIn(1, 5)
        val matched = st == "ssdp:all" ||
            st == deviceType || st == avtService || st == cmService || st == rcService ||
            st.startsWith("upnp:rootdevice")
        if (!matched) return
        sendResponse(src, srcPort, st, mx)
    }

    private fun sendResponse(dst: InetAddress, dstPort: Int, st: String, mx: Int) {
        val location = location()
        val lines = buildString {
            append("HTTP/1.1 200 OK\r\n")
            append("CACHE-CONTROL: max-age=").append(mx).append("\r\n")
            append("LOCATION: ").append(location).append("\r\n")
            append("SERVER: AirPlayTV/1.0 UPnP/1.0 DLNADOC/1.50\r\n")
            append("ST: ").append(st).append("\r\n")
            append("USN: uuid:").append(uuid).append("::").append(st).append("\r\n")
            append("EXT:\r\n\r\n")
        }
        sendUdp(lines.toByteArray(StandardCharsets.UTF_8), dst, dstPort)
    }

    private fun notifyLoop() {
        while (running.get()) {
            sendAlive()
            try { Thread.sleep(3000) } catch (_: InterruptedException) { return }
        }
    }

    private fun sendAlive() {
        val lines = buildString {
            append("NOTIFY * HTTP/1.1\r\n")
            append("HOST: ").append(group).append(':').append(port).append("\r\n")
            append("CACHE-CONTROL: max-age=1800\r\n")
            append("LOCATION: ").append(location()).append("\r\n")
            append("SERVER: AirPlayTV/1.0 UPnP/1.0 DLNADOC/1.50\r\n")
            append("NT: ").append(deviceType).append("\r\n")
            append("NTS: ssdp:alive\r\n")
            append("USN: uuid:").append(uuid).append("::").append(deviceType).append("\r\n\r\n")
        }
        runCatching { sendUdp(lines.toByteArray(StandardCharsets.UTF_8), InetAddress.getByName(group), port) }
    }

    private fun sendUdp(data: ByteArray, dst: InetAddress, dstPort: Int) {
        runCatching {
            DatagramSocket().use { s ->
                s.broadcast = true
                s.send(DatagramPacket(data, data.size, dst, dstPort))
            }
        }
    }

    private fun location(): String = "http://${localAddress()}:$httpPort/rootDesc.xml"

    private fun localAddress(): String {
        runCatching {
            DatagramSocket().use { s ->
                s.connect(InetAddress.getByName("8.8.8.8"), 80)
                return s.localAddress?.hostAddress ?: "127.0.0.1"
            }
        }
        return "127.0.0.1"
    }
}
