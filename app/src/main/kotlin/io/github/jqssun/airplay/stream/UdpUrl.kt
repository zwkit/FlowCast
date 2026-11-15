package io.github.jqssun.airplay.stream

/**
 * Parsing for `udp://` and `rtp://`, kept apart from [UdpStreamDataSource] so the pure Kotlin unit
 * tests can use it without pulling in android.net.Uri.
 */
object UdpUrl {

    /** host (null = bind to every interface), port, and whether the host is a multicast group. */
    data class Target(val host: String?, val port: Int, val multicast: Boolean)

    /**
     * Accepts the ffmpeg spellings: `udp://:5004`, `udp://@239.1.1.1:1234`,
     * `udp://192.168.0.9:5004`. A port is mandatory — unlike HTTP there is nothing to default to.
     */
    fun parse(raw: String): Target? {
        val s = raw.trim()
        val idx = s.indexOf("://")
        if (idx < 0) return null
        val authority = s.substring(idx + 3).substringBefore('/').substringBefore('?')
        val hostPart = authority.substringAfterLast('@').substringBefore(':')
            .trim().trimStart('[').trimEnd(']')
        val portPart = authority.substringAfter(':', "")
        val port = portPart.toIntOrNull() ?: return null
        if (port !in 1..65535) return null
        val host = hostPart.takeIf { it.isNotEmpty() }
        return Target(host, port, host?.let { isMulticastAddress(it) } ?: false)
    }

    fun isMulticastAddress(host: String): Boolean {
        // IPv6 multicast starts with ff00::/8; anything unparsable is treated as unicast
        if (host.contains(':')) return host.startsWith("ff", ignoreCase = true)
        val first = host.substringBefore('.').toIntOrNull() ?: return false
        return first in 224..239
    }
}
