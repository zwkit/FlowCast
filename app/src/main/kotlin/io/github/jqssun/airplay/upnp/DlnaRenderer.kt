package io.github.jqssun.airplay.upnp

import android.content.Context
import android.media.AudioManager
import android.util.Log
import kotlin.math.roundToInt
import android.util.Xml
import org.xmlpull.v1.XmlPullParser

/** Playback sink used by the DLNA renderer. Implemented by the AirPlayService. */
interface DlnaPlayback {
    fun play(uri: String, title: String?)
    fun pause()
    fun resume()
    fun stop()
    fun seek(seconds: Long)
    fun positionSeconds(): Long
    fun durationSeconds(): Long
    fun isPlaying(): Boolean
}

/** UPnP/DLNA MediaRenderer: SSDP discovery + HTTP device description + SOAP AVTransport control. */
class DlnaRenderer(
    private val context: Context,
    private val httpPort: Int,
    udn: String,
    private val friendlyNameProvider: () -> String,
    private val playback: DlnaPlayback,
) {
    // UDN must be unique per install: a shared one makes every receiver on the LAN look
    // like the same UPnP device to controllers (they key on UDN/USN), which is exactly how
    // a "cast to A" ends up playing on B
    private val uuid: String = udn
    private var httpServer: DlnaHttpServer? = null
    private var ssdp: SsdpAdvertiser? = null

    // DLNA controllers read/set the renderer volume via RenderingControl. We track the last
    // SetVolume value and reflect it back from GetVolume (DLNA contract), and also apply it to
    // the device STREAM_MUSIC volume while DLNA is the active playback source.
    private val audioManager by lazy { context.getSystemService(Context.AUDIO_SERVICE) as AudioManager }
    private var dlnaVolume = 100 // 0..100, last SetVolume (defaults to full)
    private var dlnaMuted = false

    private val deviceType = "urn:schemas-upnp-org:device:MediaRenderer:1"
    private val avtService = "urn:schemas-upnp-org:service:AVTransport:1"
    private val cmService = "urn:schemas-upnp-org:service:ConnectionManager:1"
    private val rcService = "urn:schemas-upnp-org:service:RenderingControl:1"
    private val controlNs = "urn:schemas-upnp-org:control-1-0"

    fun start() {
        if (httpServer != null) return
        try {
            val server = DlnaHttpServer(httpPort, ::handleRequest)
            server.start()
            httpServer = server
            ssdp = SsdpAdvertiser(context, uuid, httpPort).also { it.start() }
            Log.i(TAG, "DLNA renderer started on port $httpPort (uuid $uuid)")
        } catch (e: Exception) {
            Log.w(TAG, "DLNA start failed", e)
            stop()
        }
    }

    fun stop() {
        ssdp?.stop(); ssdp = null
        httpServer?.stop(); httpServer = null
    }

    private fun handleRequest(method: String, path: String, headers: Map<String, String>, body: String): HttpResponse {
        val full = path.substringBefore('?')
        return when {
            full == "/rootDesc.xml" || full == "/description.xml" || full == "/" ->
                HttpResponse(200, "OK", "text/xml; charset=utf-8", rootDescription())
            method == "POST" && full.endsWith("/control/AVTransport") ->
                handleSoap(body, avtService, "AVTransport")
            method == "POST" && full.endsWith("/control/ConnectionManager") ->
                handleSoap(body, cmService, "ConnectionManager")
            method == "POST" && full.endsWith("/control/RenderingControl") ->
                handleSoap(body, rcService, "RenderingControl")
            else -> HttpResponse(404, "Not Found", "text/plain", "Not Found")
        }
    }

    // ---------- device description ----------

    private fun rootDescription(): String {
        val name = friendlyNameProvider().replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
        val model = "AirPlay投屏"
        return """<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0">
  <specVersion><major>1</major><minor>0</minor></specVersion>
  <device>
    <deviceType>$deviceType</deviceType>
    <friendlyName>$name</friendlyName>
    <manufacturer>jqssun</manufacturer>
    <modelName>$model</modelName>
    <modelNumber>1.0</modelNumber>
    <UDN>uuid:$uuid</UDN>
    <serviceList>
      <service>
        <serviceType>$avtService</serviceType>
        <serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>
        <SCPDURL>/scpd/AVTransport.xml</SCPDURL>
        <controlURL>/control/AVTransport</controlURL>
        <eventSubURL>/event/AVTransport</eventSubURL>
      </service>
      <service>
        <serviceType>$cmService</serviceType>
        <serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>
        <SCPDURL>/scpd/ConnectionManager.xml</SCPDURL>
        <controlURL>/control/ConnectionManager</controlURL>
        <eventSubURL>/event/ConnectionManager</eventSubURL>
      </service>
      <service>
        <serviceType>$rcService</serviceType>
        <serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId>
        <SCPDURL>/scpd/RenderingControl.xml</SCPDURL>
        <controlURL>/control/RenderingControl</controlURL>
        <eventSubURL>/event/RenderingControl</eventSubURL>
      </service>
    </serviceList>
  </device>
</root>"""
    }

    // ---------- SOAP handling ----------

    private fun handleSoap(body: String, serviceType: String, service: String): HttpResponse {
        val action = extractAction(body) ?: return soapError(501, "Action not implemented")
        val args = parseArgs(body)
        val instance = args["InstanceID"]?.toIntOrNull() ?: 0
        return try {
            when (service) {
                "AVTransport" -> handleAvt(action, args, instance)
                "ConnectionManager" -> handleCm(action, args)
                "RenderingControl" -> handleRc(action, args)
                else -> soapError(501, "Action not implemented")
            }
        } catch (e: Exception) {
            Log.w(TAG, "SOAP $service/$action failed", e)
            soapError(501, "Action Failed")
        }
    }

    private fun handleAvt(action: String, args: Map<String, String>, instance: Int): HttpResponse {
        return when (action) {
            "SetAVTransportURI" -> {
                val uri = args["CurrentURI"] ?: return soapError(402, "Invalid Args")
                playback.stop()
                playback.play(uri, args["CurrentURIMetaData"]?.let { titleFromMeta(it) })
                HttpResponse(200, "OK", "text/xml; charset=\"utf-8\"", soapResponse("SetAVTransportURIResponse", avtService, ""))
            }
            "Play" -> {
                playback.resume()
                HttpResponse(200, "OK", "text/xml; charset=\"utf-8\"", soapResponse("PlayResponse", avtService, ""))
            }
            "Pause" -> {
                playback.pause()
                HttpResponse(200, "OK", "text/xml; charset=\"utf-8\"", soapResponse("PauseResponse", avtService, ""))
            }
            "Stop" -> {
                playback.stop()
                HttpResponse(200, "OK", "text/xml; charset=\"utf-8\"", soapResponse("StopResponse", avtService, ""))
            }
            "Seek" -> {
                val target = args["Target"]?.let { parseHms(it) } ?: return soapError(402, "Invalid Args")
                playback.seek(target)
                HttpResponse(200, "OK", "text/xml; charset=\"utf-8\"", soapResponse("SeekResponse", avtService, ""))
            }
            "GetTransportInfo" -> {
                // the TV's own remote can pause a DLNA cast, so a not-playing player with a
                // position is PAUSED_PLAYBACK, not STOPPED — the phone's UI mirrors this string
                val state = when {
                    playback.isPlaying() && playback.durationSeconds() <= 0 && playback.positionSeconds() <= 0 -> "TRANSITIONING"
                    playback.isPlaying() -> "PLAYING"
                    playback.positionSeconds() > 0 -> "PAUSED_PLAYBACK"
                    else -> "STOPPED"
                }
                HttpResponse(200, "OK", "text/xml; charset=\"utf-8\"",
                    soapResponse("GetTransportInfoResponse", avtService,
                        "<CurrentTransportState>$state</CurrentTransportState>" +
                        "<CurrentTransportStatus>OK</CurrentTransportStatus><CurrentSpeed>1</CurrentSpeed>"))
            }
            "GetPositionInfo" -> {
                val p = playback.positionSeconds()
                val d = playback.durationSeconds()
                HttpResponse(200, "OK", "text/xml; charset=\"utf-8\"",
                    soapResponse("GetPositionInfoResponse", avtService,
                        "<Track>0</Track><TrackDuration>${fmt(d)}</TrackDuration>" +
                        "<TrackMetaData></TrackMetaData><TrackURI></TrackURI>" +
                        "<RelTime>${fmt(p)}</RelTime><AbsTime>${fmt(p)}</AbsTime><RelCount>0</RelCount>" +
                        "<AbsCount>0</AbsCount>"))
            }
            "GetMediaInfo" -> {
                val d = playback.durationSeconds()
                HttpResponse(200, "OK", "text/xml; charset=\"utf-8\"",
                    soapResponse("GetMediaInfoResponse", avtService,
                        "<NrTracks>1</NrTracks><MediaDuration>${fmt(d)}</MediaDuration>" +
                        "<CurrentURI></CurrentURI><CurrentURIMetaData></CurrentURIMetaData>" +
                        "<NextURI></NextURI><NextURIMetaData></NextURIMetaData><PlayMedium>NONE</PlayMedium>" +
                        "<RecordMedium>NONE</RecordMedium><WriteStatus>NOT_WRITABLE</WriteStatus>"))
            }
            "GetAVTransportURI" ->
                HttpResponse(200, "OK", "text/xml; charset=\"utf-8\"",
                    soapResponse("GetAVTransportURIResponse", avtService, "<CurrentURI></CurrentURI><CurrentURIMetaData></CurrentURIMetaData>"))
            "GetTransportSettings" ->
                HttpResponse(200, "OK", "text/xml; charset=\"utf-8\"",
                    soapResponse("GetTransportSettingsResponse", avtService, "<PlayMode>NORMAL</PlayMode><RecQualityMode>0</RecQualityMode>"))
            "SetNextAVTransportURI", "GetDeviceCapabilities", "GetCurrentTransportActions" ->
                HttpResponse(200, "OK", "text/xml; charset=\"utf-8\"", soapResponse("${action}Response", avtService, ""))
            else -> soapError(501, "Action not implemented")
        }
    }

    private fun handleCm(action: String, args: Map<String, String>): HttpResponse {
        return when (action) {
            "GetProtocolInfo" ->
                HttpResponse(200, "OK", "text/xml; charset=\"utf-8\"",
                    soapResponse("GetProtocolInfoResponse", cmService,
                        "<Source>http-get:*:image/jpeg:DLNA.ORG_PN=JPEG_TN;http-get:*:image/png:*</Source>" +
                        "<Sink>http-get:*:video/mp4:*;http-get:*:video/x-matroska:*;http-get:*:video/x-msvideo:*;http-get:*:video/quicktime:*;http-get:*:video/webm:*;http-get:*:video/3gpp:*;http-get:*:audio/mpeg:*;http-get:*:audio/mp4:*;http-get:*:audio/x-wav:*;http-get:*:audio/x-ms-wma:*;http-get:*:audio/ogg:*;http-get:*:application/vnd.apple.mpegurl:*;http-get:*:application/x-mpegURL:*</Sink>"))
            "GetCurrentConnectionIDs" ->
                HttpResponse(200, "OK", "text/xml; charset=\"utf-8\"",
                    soapResponse("GetCurrentConnectionIDsResponse", cmService, "<ConnectionIDs>0</ConnectionIDs>"))
            "GetCurrentConnectionInfo" ->
                HttpResponse(200, "OK", "text/xml; charset=\"utf-8\"",
                    soapResponse("GetCurrentConnectionInfoResponse", cmService,
                        "<RcsID>0</RcsID><AVTransportID>0</AVTransportID><ProtocolInfo></ProtocolInfo><PeerConnectionManager></PeerConnectionManager><PeerConnectionID>-1</PeerConnectionID><Direction>Input</Direction><Status>OK</Status>"))
            else -> soapError(501, "Action not implemented")
        }
    }

    private fun handleRc(action: String, args: Map<String, String>): HttpResponse {
        return when (action) {
            "GetVolume" -> {
                val ch = args["Channel"] ?: "Master"
                HttpResponse(200, "OK", "text/xml; charset=\"utf-8\"",
                    soapResponse("GetVolumeResponse", rcService, "<CurrentVolume>$currentVolume</CurrentVolume><Channel>$ch</Channel>"))
            }
            "SetVolume" -> {
                val desired = args["DesiredVolume"]?.toIntOrNull()?.coerceIn(0, 100) ?: dlnaVolume
                dlnaVolume = desired
                if (!dlnaMuted) {
                    val max = audioManager.getStreamMaxVolume(AudioManager.STREAM_MUSIC)
                    val idx = (desired * max / 100f).roundToInt().coerceIn(0, max)
                    runCatching { audioManager.setStreamVolume(AudioManager.STREAM_MUSIC, idx, 0) }
                }
                HttpResponse(200, "OK", "text/xml; charset=\"utf-8\"", soapResponse("SetVolumeResponse", rcService, ""))
            }
            "GetMute" -> {
                val ch = args["Channel"] ?: "Master"
                HttpResponse(200, "OK", "text/xml; charset=\"utf-8\"",
                    soapResponse("GetMuteResponse", rcService, "<CurrentMute>${if (dlnaMuted) 1 else 0}</CurrentMute><Channel>$ch</Channel>"))
            }
            "SetMute" -> {
                dlnaMuted = args["DesiredMute"]?.trim() == "1"
                HttpResponse(200, "OK", "text/xml; charset=\"utf-8\"", soapResponse("SetMuteResponse", rcService, ""))
            }
            else -> soapError(501, "Action not implemented")
        }
    }

    private val currentVolume get() = if (dlnaMuted) 0 else dlnaVolume

    // ---------- SOAP helpers ----------

    private fun extractAction(body: String): String? {
        val parser = Xml.newPullParser()
        parser.setInput(body.reader())
        val envelopeNs = "http://schemas.xmlsoap.org/soap/envelope/"
        var event = parser.eventType
        while (event != XmlPullParser.END_DOCUMENT) {
            if (event == XmlPullParser.START_TAG) {
                val ns = parser.namespace
                val local = parser.name?.substringAfter(':') ?: ""
                // the action element is the first non-envelope element (e.g. <u:SetAVTransportURI xmlns:u="...">)
                if (ns != null && ns != envelopeNs && local != "Body" && local != "Envelope") {
                    return local
                }
            }
            event = parser.next()
        }
        return null
    }

    private fun parseArgs(body: String): Map<String, String> {
        val result = LinkedHashMap<String, String>()
        val parser = Xml.newPullParser()
        parser.setInput(body.reader())
        val envelopeNs = "http://schemas.xmlsoap.org/soap/envelope/"
        var event = parser.eventType
        var inAction = false
        while (event != XmlPullParser.END_DOCUMENT) {
            if (event == XmlPullParser.START_TAG) {
                val ns = parser.namespace ?: ""
                val local = parser.name?.substringAfter(':') ?: ""
                if (!inAction && ns != envelopeNs && local != "Body" && local != "Envelope") {
                    inAction = true
                } else if (inAction) {
                    // parameter leaf element: read its text content (URLs, InstanceID, ...)
                    val text = parser.nextText().trim()
                    if (text.isNotEmpty()) result[local] = text
                    event = parser.eventType
                    continue
                }
            }
            event = parser.next()
        }
        return result
    }

    private fun soapResponse(responseName: String, serviceType: String, body: String): String =
        """<?xml version="1.0" encoding="utf-8"?>
<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">
  <s:Body>
    <u:$responseName xmlns:u="$serviceType">$body</u:$responseName>
  </s:Body>
</s:Envelope>"""

    private fun soapError(code: Int, desc: String): HttpResponse =
        HttpResponse(500, "Internal Server Error", "text/xml; charset=\"utf-8\"",
            """<?xml version="1.0" encoding="utf-8"?>
<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">
  <s:Body><s:Fault>
    <faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring>
    <detail><UPnPError xmlns="$controlNs"><errorCode>$code</errorCode><errorDescription>$desc</errorDescription></UPnPError></detail>
  </s:Fault></s:Body>
</s:Envelope>""")

    private fun titleFromMeta(meta: String): String? {
        // DIDL-Lite XML carrying dc:title
        return runCatching {
            val parser = Xml.newPullParser()
            parser.setInput(meta.reader())
            var event = parser.eventType
            while (event != XmlPullParser.END_DOCUMENT) {
                if (event == XmlPullParser.START_TAG && parser.name?.substringAfter(':') == "title") {
                    return parser.nextText().trim().ifEmpty { null }
                }
                event = parser.next()
            }
            null
        }.getOrNull()
    }

    private fun parseHms(s: String): Long {
        val parts = s.split(':')
        return when (parts.size) {
            3 -> parts[0].toLongOrNull()?.times(3600)?.plus(parts[1].toLongOrNull()?.times(60) ?: 0)?.plus(parts[2].toLongOrNull() ?: 0) ?: 0
            2 -> parts[0].toLongOrNull()?.times(60)?.plus(parts[1].toLongOrNull() ?: 0) ?: 0
            else -> s.toLongOrNull() ?: 0
        }
    }

    private fun fmt(seconds: Long): String {
        val s = seconds.coerceAtLeast(0)
        return "%02d:%02d:%02d".format(s / 3600, (s % 3600) / 60, s % 60)
    }

    companion object {
        private const val TAG = "DlnaRenderer"
    }
}
