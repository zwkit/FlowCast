package io.github.jqssun.airplay.renderer

import android.graphics.SurfaceTexture
import android.opengl.EGL14
import android.opengl.EGLSurface
import android.opengl.GLES11Ext
import android.opengl.GLES20
import android.opengl.Matrix
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer

// decodes into an app-owned SurfaceTexture that outlives display surface
// gl thread blits it to any surface attached, so fullscreen toggles re-point display without restarting codec
class VideoPipeline {

    private val lock = Object()
    private var thread: Thread? = null
    @Volatile private var running = false

    private var egl: EglCore? = null
    private var window: EGLSurface = EGL14.EGL_NO_SURFACE
    private var winW = 0
    private var winH = 0

    private var oesTex = 0
    private var program = 0
    private var aPos = 0
    private var aTex = 0
    private var uTexMatrix = 0
    private val texMatrix = FloatArray(16)
    private var hasFrame = false

    private var surfaceTexture: SurfaceTexture? = null
    var inputSurface: Surface? = null; private set

    @Volatile private var frameAvailable = false
    private var pendingDisplay: Surface? = null
    private var displayDirty = false
    // set from the main thread to black out the current display surface (clear a stale last
    // frame left by a just-ended mirroring/playback session); handled on the render thread
    @Volatile private var clearRequested = false
    @Volatile private var videoW = 0
    @Volatile private var videoH = 0

    // actual frames blitted to the display surface per second (true on-screen frame rate);
    // distinct from the input-frame counter in VideoRenderer.feedFrame which counts network
    // arrival (including dropped frames) and thus over-reports the visible rate.
    @Volatile var renderFps = 0; private set
    private var _renderedThisSec = 0
    private var _lastRenderCountReset = 0L

    // manual pacing to the display refresh rate. eglSwapInterval is not honored on every
    // device/driver; when it is ignored the render loop can chase a decoder buffer backlog and
    // blit thousands of frames per second (the measured fps climbs to 1000+). Throttling the
    // loop ourselves caps the blit rate at the refresh rate and makes renderFps reflect what is
    // actually displayed. updateTexImage() always picks the newest queued frame, so throttling
    // just drops stale back-logged frames instead of rendering them all.
    @Volatile private var _targetIntervalNs = 16_666_667L // ~60Hz default
    private var _lastRenderNs = 0L

    fun setRefreshRate(hz: Float) {
        val intervalNs = if (hz > 0f) (1_000_000_000.0 / hz).toLong() else 16_666_667L
        _targetIntervalNs = intervalNs.coerceIn(8_000_000L, 50_000_000L) // 20Hz..125Hz
    }

    private fun _throttleRender() {
        val now = System.nanoTime()
        val elapsed = now - _lastRenderNs
        val remaining = _targetIntervalNs - elapsed
        if (remaining > 0) {
            try {
                Thread.sleep(remaining / 1_000_000L)
            } catch (_: InterruptedException) {
                Thread.currentThread().interrupt()
            }
        }
        _lastRenderNs = System.nanoTime()
    }

    fun start() = synchronized(lock) {
        if (running) return@synchronized
        running = true
        thread = Thread({ _loop() }, "VideoPipeline").also { it.start() }
        while (inputSurface == null && running) lock.wait()
    }

    fun setDisplaySurface(surface: Surface?) = synchronized(lock) {
        pendingDisplay = surface
        displayDirty = true
        lock.notifyAll()
    }

    // request the render thread to paint the current display surface solid black, wiping any
    // stale last frame (e.g. the residual phone-screen grab on TV after a mirror ends). Safe to
    // call from any thread; the actual blit happens on the render thread under the GL context.
    fun clearDisplay() = synchronized(lock) {
        clearRequested = true
        lock.notifyAll()
    }

    fun setVideoSize(w: Int, h: Int) {
        videoW = w
        videoH = h
        surfaceTexture?.setDefaultBufferSize(w, h)
    }

    fun release() {
        synchronized(lock) {
            if (!running) return
            running = false
            lock.notifyAll()
        }
        thread?.join()
        thread = null
    }

    private fun _loop() {
        try {
            egl = EglCore()
            _initGl()
        } catch (e: Exception) {
            Log.e(TAG, "GL init failed", e)
            synchronized(lock) { running = false; lock.notifyAll() }
            return
        }
        Matrix.setIdentityM(texMatrix, 0)
        synchronized(lock) {
            inputSurface = Surface(surfaceTexture)
            lock.notifyAll()
        }
        while (true) {
            var newDisplay: Surface? = null
            var displayChanged = false
            var doFrame = false
            var doClear = false
            synchronized(lock) {
                while (running && !frameAvailable && !displayDirty && !clearRequested) lock.wait()
                if (running && displayDirty) {
                    newDisplay = pendingDisplay
                    displayChanged = true
                    displayDirty = false
                }
                if (running && clearRequested) {
                    clearRequested = false
                    doClear = true
                }
                if (running && frameAvailable) {
                    frameAvailable = false
                    doFrame = true
                }
            }
            if (!running) break
            // a backgrounded/stopped activity can hand us an invalid display surface; a GL
            // failure there must NOT escape the thread (an uncaught renderer exception kills the
            // whole process via the default uncaught handler) -- reset and retry instead
            try {
                if (displayChanged) _bindDisplay(newDisplay)
                if (doClear) _renderBlack()
                if (doFrame) {
                    _consumeAndDraw()
                    // pace blits to the refresh rate so a backlog can't inflate renderFps to 1000+
                    _throttleRender()
                }
            } catch (e: Exception) {
                Log.e(TAG, "Render error, resetting display", e)
                _resetDisplayForError()
            }
        }
        _releaseGl()
    }

    private fun _resetDisplayForError() {
        synchronized(lock) {
            try {
                egl?.let {
                    if (window != EGL14.EGL_NO_SURFACE) {
                        it.makeCurrent()
                        it.destroySurface(window)
                    }
                }
            } catch (_: Exception) {}
            window = EGL14.EGL_NO_SURFACE
            // leave frameAvailable as-is; a later display bind repaints the last frame
        }
    }

    private fun _bindDisplay(surface: Surface?) {
        val egl = egl ?: return
        if (window != EGL14.EGL_NO_SURFACE) {
            egl.makeCurrent()
            egl.destroySurface(window)
            window = EGL14.EGL_NO_SURFACE
        }
        if (surface == null || !surface.isValid) return
        window = egl.windowSurface(surface)
        if (window == EGL14.EGL_NO_SURFACE) {
            Log.w(TAG, "eglCreateWindowSurface failed: ${EGL14.eglGetError()}")
            return
        }
        egl.makeCurrent(window)
        egl.setSwapInterval(1)
        winW = egl.query(window, EGL14.EGL_WIDTH)
        winH = egl.query(window, EGL14.EGL_HEIGHT)
        // an idle source sends no new frames, so repaint last one or new surface stays black
        if (hasFrame) _render()
    }

    private fun _consumeAndDraw() {
        val st = surfaceTexture ?: return
        val egl = egl ?: return
        if (window == EGL14.EGL_NO_SURFACE) {
            // no display: keep consuming so decoder doesn't stall
            egl.makeCurrent()
            st.updateTexImage()
            hasFrame = true
            return
        }
        egl.makeCurrent(window)
        st.updateTexImage()
        st.getTransformMatrix(texMatrix)
        hasFrame = true
        _render()
        _trackRender()
    }

    private fun _trackRender() {
        val now = System.currentTimeMillis()
        if (now - _lastRenderCountReset >= 1000) {
            renderFps = _renderedThisSec
            Log.i(TAG, "renderFps=$renderFps paced=${1_000_000_000 / _targetIntervalNs}Hz")
            _renderedThisSec = 0
            _lastRenderCountReset = now
        }
        _renderedThisSec++
    }

    private fun _render() {
        GLES20.glViewport(0, 0, winW, winH)
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
        GLES20.glUseProgram(program)
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oesTex)
        GLES20.glUniformMatrix4fv(uTexMatrix, 1, false, texMatrix, 0)
        GLES20.glEnableVertexAttribArray(aPos)
        GLES20.glVertexAttribPointer(aPos, 2, GLES20.GL_FLOAT, false, 0, POS)
        GLES20.glEnableVertexAttribArray(aTex)
        GLES20.glVertexAttribPointer(aTex, 2, GLES20.GL_FLOAT, false, 0, TEX)
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
        GLES20.glDisableVertexAttribArray(aPos)
        GLES20.glDisableVertexAttribArray(aTex)
        egl?.swap(window)
    }

    // paint the whole display surface black to wipe a stale last frame after a session ends.
    // hasFrame is cleared so _bindDisplay won't repaint the old frame onto a fresh surface.
    private fun _renderBlack() {
        val e = egl ?: return
        if (window == EGL14.EGL_NO_SURFACE) return
        e.makeCurrent(window)
        GLES20.glViewport(0, 0, winW, winH)
        GLES20.glClearColor(0f, 0f, 0f, 1f)
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
        e.swap(window)
        hasFrame = false
    }

    private fun _initGl() {
        program = _buildProgram()
        aPos = GLES20.glGetAttribLocation(program, "aPos")
        aTex = GLES20.glGetAttribLocation(program, "aTex")
        uTexMatrix = GLES20.glGetUniformLocation(program, "uTexMatrix")
        val tex = IntArray(1)
        GLES20.glGenTextures(1, tex, 0)
        oesTex = tex[0]
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oesTex)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
        surfaceTexture = SurfaceTexture(oesTex).also {
            if (videoW > 0 && videoH > 0) it.setDefaultBufferSize(videoW, videoH)
            it.setOnFrameAvailableListener {
                synchronized(lock) { frameAvailable = true; lock.notifyAll() }
            }
        }
    }

    private fun _buildProgram(): Int {
        val vs = _shader(GLES20.GL_VERTEX_SHADER, VERT)
        val fs = _shader(GLES20.GL_FRAGMENT_SHADER, FRAG)
        return GLES20.glCreateProgram().also {
            GLES20.glAttachShader(it, vs)
            GLES20.glAttachShader(it, fs)
            GLES20.glLinkProgram(it)
            GLES20.glDeleteShader(vs)
            GLES20.glDeleteShader(fs)
        }
    }

    private fun _shader(type: Int, src: String): Int {
        return GLES20.glCreateShader(type).also {
            GLES20.glShaderSource(it, src)
            GLES20.glCompileShader(it)
            val ok = IntArray(1)
            GLES20.glGetShaderiv(it, GLES20.GL_COMPILE_STATUS, ok, 0)
            if (ok[0] == 0) Log.e(TAG, "shader compile failed: ${GLES20.glGetShaderInfoLog(it)}")
        }
    }

    private fun _releaseGl() {
        surfaceTexture?.release()
        surfaceTexture = null
        inputSurface?.release()
        inputSurface = null
        egl?.let {
            if (window != EGL14.EGL_NO_SURFACE) it.destroySurface(window)
            it.close()
        }
        egl = null
        window = EGL14.EGL_NO_SURFACE
    }

    companion object {
        private const val TAG = "VideoPipeline"

        private val POS = _fb(floatArrayOf(-1f, -1f, 1f, -1f, -1f, 1f, 1f, 1f))
        private val TEX = _fb(floatArrayOf(0f, 0f, 1f, 0f, 0f, 1f, 1f, 1f))

        private fun _fb(a: FloatArray): FloatBuffer =
            ByteBuffer.allocateDirect(a.size * 4).order(ByteOrder.nativeOrder())
                .asFloatBuffer().apply { put(a); position(0) }

        private const val VERT =
            "attribute vec2 aPos;\n" +
            "attribute vec2 aTex;\n" +
            "uniform mat4 uTexMatrix;\n" +
            "varying vec2 vTex;\n" +
            "void main() {\n" +
            "  gl_Position = vec4(aPos, 0.0, 1.0);\n" +
            "  vTex = (uTexMatrix * vec4(aTex, 0.0, 1.0)).xy;\n" +
            "}\n"

        private const val FRAG =
            "#extension GL_OES_EGL_image_external : require\n" +
            "precision mediump float;\n" +
            "varying vec2 vTex;\n" +
            "uniform samplerExternalOES sTex;\n" +
            "void main() {\n" +
            "  gl_FragColor = texture2D(sTex, vTex);\n" +
            "}\n"
    }
}
