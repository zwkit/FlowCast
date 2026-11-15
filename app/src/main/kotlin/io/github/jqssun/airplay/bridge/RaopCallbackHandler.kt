package io.github.jqssun.airplay.bridge

import java.nio.ByteBuffer

interface RaopCallbackHandler {
    // zero-copy video delivery: buf is a reusable direct ByteBuffer owned by the
    // native side (len valid bytes at position 0). Consume it — i.e. copy into the
    // decoder — before returning; it is refilled with the next frame.
    fun onVideoData(sessionId: Long, buf: ByteBuffer, len: Int, ntpTimeNs: Long, isH265: Boolean)
    fun onAudioFormat(sessionId: Long, ct: Int, spf: Int, usingScreen: Boolean)
    fun onVideoSize(sessionId: Long, srcW: Float, srcH: Float, w: Float, h: Float)
    // sender identity reported during pair-setup (model e.g. "iPhone15,2")
    fun onClientInfo(sessionId: Long, model: String, name: String)
    fun onVolumeChange(volume: Float)
    fun onClientVolume(): Float
    fun onAudioTeardown()
    fun onConnectionInit(sessionId: Long)
    fun onConnectionDestroy(sessionId: Long)
    fun onConnectionReset(sessionId: Long, reason: Int)
    fun onDisplayPin(pin: String)
    fun onMetadata(data: ByteArray)
    fun onCoverArt(data: ByteArray)
    fun onProgress(start: Long, curr: Long, end: Long)
    fun onDacpId(dacpId: String, activeRemote: String)
    fun onMirrorRunning(sessionId: Long, running: Boolean)
    // video (hls), distinct from mirroring and raop audio
    fun onVideoPlay(location: String, startPositionSeconds: Float)
    fun onVideoScrub(positionSeconds: Float)
    fun onVideoRate(rate: Float)
    fun onVideoStop()
    // fired per sender GET /playback-info poll; polling starts before /play
    fun onVideoSessionPoll()
}
