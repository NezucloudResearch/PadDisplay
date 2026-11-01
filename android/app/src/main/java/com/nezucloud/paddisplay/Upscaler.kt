// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import android.content.Context
import android.graphics.SurfaceTexture
import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.EGLExt
import android.opengl.GLES11Ext
import android.opengl.GLES30
import android.os.Handler
import android.os.HandlerThread
import android.os.Process
import android.os.SystemClock
import android.util.Log
import android.view.Surface
import java.util.concurrent.ExecutionException
import java.util.concurrent.FutureTask
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong

/**
 * Shows the decoded picture upscaled to the screen with Snapdragon GSR, for when the PC renders below the panel's
 * resolution. GSR is one shader pass: a 12-tap Lanczos-like filter with edge-aware sharpening, not a neural network
 * (one of those takes longer than a frame on a tablet GPU). It is plain OpenGL ES 3.1, so Adreno (Snapdragon) and
 * Mali (MediaTek) both run it.
 *
 * The decoder draws into [inputSurface]; everything GL happens on the `pd-upscale` thread. This path has one buffer
 * hop more than the decoder drawing straight into the SurfaceView, so it is only used when there is something to
 * upscale. Throws from the constructor if the GPU cannot run it; the caller then shows the picture directly.
 */
class Upscaler(context: Context, outSurface: Surface) {

    /** CPU time per shown frame (latch + both passes + swap); the GPU's own time shows in SurfaceFlinger's latency. */
    val drawUsTotal = AtomicLong()
    val drawCount = AtomicInteger()

    // Urgent-display priority: this thread now stands between the decoder and the screen.
    private val thread = HandlerThread("pd-upscale", Process.THREAD_PRIORITY_URGENT_DISPLAY).apply { start() }
    private val handler = Handler(thread.looper)

    // Everything below belongs to the pd-upscale thread.
    private var display = EGL14.EGL_NO_DISPLAY
    private var eglContext = EGL14.EGL_NO_CONTEXT
    private var window = EGL14.EGL_NO_SURFACE
    private var surfaceTexture: SurfaceTexture? = null
    private var decoderInput: Surface? = null // the same object as inputSurface, released here
    private var oesTexture = 0
    private var copyProgram = 0
    private var gsrProgram = 0
    private var texMatrixLocation = 0
    private var viewportInfoLocation = 0
    private var frameTexture = 0
    private var framebuffer = 0
    private var videoW = 0
    private var videoH = 0
    private var viewW = 0
    private var viewH = 0
    private var pendingFrames = 0
    private var released = false
    private var windowLost = false
    private var drawErrors = 0
    private val texMatrix = FloatArray(16)
    private val colorAttachment = intArrayOf(GLES30.GL_COLOR_ATTACHMENT0)
    private val drawNewest = Runnable { drawNewestFrame() }

    val inputSurface: Surface

    init {
        val setUp = FutureTask { setUp(context.applicationContext, outSurface) }
        handler.post(setUp)
        inputSurface = try {
            setUp.get()
        } catch (e: ExecutionException) {
            release()
            throw e.cause ?: e
        }
    }

    /** The stream's size, from HostHello. Frames are not drawn before the first call. */
    fun setVideoSize(width: Int, height: Int) {
        handler.post { allocateFrame(width, height) }
    }

    /** The SurfaceView's size, from surfaceChanged. */
    fun setViewSize(width: Int, height: Int) {
        handler.post {
            viewW = width
            viewH = height
        }
    }

    /** Call after the decoder let go of [inputSurface], and before the output surface is destroyed. */
    fun release() {
        handler.post { tearDown() }
        thread.quitSafely()
        thread.join(500)
    }

    private fun setUp(context: Context, outSurface: Surface): Surface {
        val started = SystemClock.elapsedRealtime()
        display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
        val version = IntArray(2)
        check(EGL14.eglInitialize(display, version, 0, version, 1)) { "eglInitialize: 0x%x".format(EGL14.eglGetError()) }
        val config = chooseConfig()
        eglContext = EGL14.eglCreateContext(display, config, EGL14.EGL_NO_CONTEXT,
            intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 3, EGL14.EGL_NONE), 0)
        check(eglContext != EGL14.EGL_NO_CONTEXT) { "eglCreateContext: 0x%x".format(EGL14.eglGetError()) }
        window = EGL14.eglCreateWindowSurface(display, config, outSurface, intArrayOf(EGL14.EGL_NONE), 0)
        check(window != EGL14.EGL_NO_SURFACE) { "eglCreateWindowSurface: 0x%x".format(EGL14.eglGetError()) }
        check(EGL14.eglMakeCurrent(display, window, window, eglContext)) { "eglMakeCurrent: 0x%x".format(EGL14.eglGetError()) }
        // Interval 0 puts the window's buffer queue in async mode: a newer frame replaces one SurfaceFlinger has not
        // shown yet instead of queueing behind it, and eglSwapBuffers never waits for vsync.
        EGL14.eglSwapInterval(display, 0)
        viewW = querySurface(EGL14.EGL_WIDTH)
        viewH = querySurface(EGL14.EGL_HEIGHT)

        copyProgram = linkProgram(COPY_VS, COPY_FS)
        texMatrixLocation = GLES30.glGetUniformLocation(copyProgram, "texMatrix")
        gsrProgram = linkProgram(QUAD_VS, gsrShader(context))
        viewportInfoLocation = GLES30.glGetUniformLocation(gsrProgram, "ViewportInfo")

        oesTexture = createTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES)
        val st = SurfaceTexture(oesTexture)
        st.setOnFrameAvailableListener({ onFrameAvailable() }, handler)
        surfaceTexture = st
        Log.i(TAG, "GSR upscaler ready in ${SystemClock.elapsedRealtime() - started} ms on ${GLES30.glGetString(GLES30.GL_RENDERER)}")
        return Surface(st).also { decoderInput = it }
    }

    private fun chooseConfig(): EGLConfig {
        val attributes = intArrayOf(
            EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8,
            EGL14.EGL_RENDERABLE_TYPE, EGLExt.EGL_OPENGL_ES3_BIT_KHR,
            EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT,
            EGL14.EGL_NONE)
        val configs = arrayOfNulls<EGLConfig>(1)
        val count = IntArray(1)
        check(EGL14.eglChooseConfig(display, attributes, 0, configs, 0, 1, count, 0) && count[0] > 0) { "no OpenGL ES 3 config" }
        return configs[0]!!
    }

    private fun querySurface(what: Int): Int {
        val value = IntArray(1)
        EGL14.eglQuerySurface(display, window, what, value, 0)
        return value[0]
    }

    // Snapdragon's GSR shader, with the changes Mali needs listed in its header.
    private fun gsrShader(context: Context): String =
        context.assets.open("sgsr1_shader_mobile.frag").bufferedReader().use { it.readText() }

    // The decoded frame (an external, often YUV texture) is copied into an RGBA texture at the stream's size first:
    // textureGather does not work on external textures. The copy is cheap at the lower resolution.
    private fun allocateFrame(width: Int, height: Int) {
        if (released || (width == videoW && height == videoH && framebuffer != 0)) return
        deleteFrame()
        videoW = width
        videoH = height
        frameTexture = createTexture(GLES30.GL_TEXTURE_2D)
        GLES30.glTexStorage2D(GLES30.GL_TEXTURE_2D, 1, GLES30.GL_RGBA8, width, height)
        val ids = IntArray(1)
        GLES30.glGenFramebuffers(1, ids, 0)
        framebuffer = ids[0]
        GLES30.glBindFramebuffer(GLES30.GL_FRAMEBUFFER, framebuffer)
        GLES30.glFramebufferTexture2D(GLES30.GL_FRAMEBUFFER, GLES30.GL_COLOR_ATTACHMENT0, GLES30.GL_TEXTURE_2D, frameTexture, 0)
        val status = GLES30.glCheckFramebufferStatus(GLES30.GL_FRAMEBUFFER)
        GLES30.glBindFramebuffer(GLES30.GL_FRAMEBUFFER, 0)
        if (status != GLES30.GL_FRAMEBUFFER_COMPLETE) {
            Log.w(TAG, "frame buffer ${width}x$height incomplete: 0x%x".format(status))
            deleteFrame()
            return
        }
        GLES30.glUseProgram(gsrProgram)
        GLES30.glUniform4f(viewportInfoLocation, 1f / width, 1f / height, width.toFloat(), height.toFloat())
    }

    private fun onFrameAvailable() {
        if (pendingFrames++ == 0) handler.post(drawNewest)
    }

    // Latches every frame that arrived since the last draw, keeping only the newest, and draws that one. An exception
    // here would end the app: the texture is abandoned or the window gone when the surfaces are torn down under it.
    private fun drawNewestFrame() {
        val st = surfaceTexture ?: return
        if (released || windowLost) return
        try {
            draw(st)
        } catch (e: RuntimeException) {
            pendingFrames = 0
            if (drawErrors++ % 120 == 0) Log.w(TAG, "draw failed ($drawErrors so far): $e")
        }
    }

    private fun draw(st: SurfaceTexture) {
        val started = System.nanoTime()
        repeat(pendingFrames) { st.updateTexImage() }
        pendingFrames = 0
        if (framebuffer == 0) return
        st.getTransformMatrix(texMatrix)

        GLES30.glBindFramebuffer(GLES30.GL_FRAMEBUFFER, framebuffer)
        // Tiled GPUs (Adreno, Mali) would otherwise load the old contents before overwriting all of them.
        GLES30.glInvalidateFramebuffer(GLES30.GL_FRAMEBUFFER, 1, colorAttachment, 0)
        GLES30.glViewport(0, 0, videoW, videoH)
        GLES30.glUseProgram(copyProgram)
        GLES30.glUniformMatrix4fv(texMatrixLocation, 1, false, texMatrix, 0)
        GLES30.glActiveTexture(GLES30.GL_TEXTURE0)
        GLES30.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oesTexture)
        GLES30.glDrawArrays(GLES30.GL_TRIANGLE_STRIP, 0, 4)

        GLES30.glBindFramebuffer(GLES30.GL_FRAMEBUFFER, 0)
        GLES30.glViewport(0, 0, viewW, viewH)
        GLES30.glUseProgram(gsrProgram)
        GLES30.glBindTexture(GLES30.GL_TEXTURE_2D, frameTexture)
        GLES30.glDrawArrays(GLES30.GL_TRIANGLE_STRIP, 0, 4)
        if (!EGL14.eglSwapBuffers(display, window)) {
            val error = EGL14.eglGetError()
            // The screen's surface is gone: nothing drawn here can be shown any more (a new one brings a new Upscaler).
            if (error == EGL14.EGL_BAD_SURFACE || error == EGL14.EGL_BAD_NATIVE_WINDOW) windowLost = true
            Log.w(TAG, "eglSwapBuffers: 0x%x".format(error))
            return
        }

        drawUsTotal.addAndGet((System.nanoTime() - started) / 1000)
        drawCount.incrementAndGet()
    }

    private fun tearDown() {
        released = true
        decoderInput?.release()
        decoderInput = null
        surfaceTexture?.release()
        surfaceTexture = null
        if (eglContext != EGL14.EGL_NO_CONTEXT) {
            deleteFrame()
            if (oesTexture != 0) GLES30.glDeleteTextures(1, intArrayOf(oesTexture), 0)
            if (copyProgram != 0) GLES30.glDeleteProgram(copyProgram)
            if (gsrProgram != 0) GLES30.glDeleteProgram(gsrProgram)
            EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
            EGL14.eglDestroyContext(display, eglContext)
        }
        if (window != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(display, window)
        // No eglTerminate: the default display is shared with the rest of the app (the UI's own renderer).
        EGL14.eglReleaseThread()
    }

    private fun deleteFrame() {
        if (framebuffer != 0) GLES30.glDeleteFramebuffers(1, intArrayOf(framebuffer), 0)
        if (frameTexture != 0) GLES30.glDeleteTextures(1, intArrayOf(frameTexture), 0)
        framebuffer = 0
        frameTexture = 0
    }

    private fun createTexture(target: Int): Int {
        val ids = IntArray(1)
        GLES30.glGenTextures(1, ids, 0)
        GLES30.glBindTexture(target, ids[0])
        GLES30.glTexParameteri(target, GLES30.GL_TEXTURE_MIN_FILTER, GLES30.GL_LINEAR)
        GLES30.glTexParameteri(target, GLES30.GL_TEXTURE_MAG_FILTER, GLES30.GL_LINEAR)
        GLES30.glTexParameteri(target, GLES30.GL_TEXTURE_WRAP_S, GLES30.GL_CLAMP_TO_EDGE)
        GLES30.glTexParameteri(target, GLES30.GL_TEXTURE_WRAP_T, GLES30.GL_CLAMP_TO_EDGE)
        return ids[0]
    }

    private fun linkProgram(vertexSource: String, fragmentSource: String): Int {
        val vertex = compileShader(GLES30.GL_VERTEX_SHADER, vertexSource)
        val fragment = compileShader(GLES30.GL_FRAGMENT_SHADER, fragmentSource)
        val program = GLES30.glCreateProgram()
        GLES30.glAttachShader(program, vertex)
        GLES30.glAttachShader(program, fragment)
        GLES30.glLinkProgram(program)
        GLES30.glDeleteShader(vertex)
        GLES30.glDeleteShader(fragment)
        val linked = IntArray(1)
        GLES30.glGetProgramiv(program, GLES30.GL_LINK_STATUS, linked, 0)
        check(linked[0] != 0) { "link: " + GLES30.glGetProgramInfoLog(program) }
        return program
    }

    private fun compileShader(type: Int, source: String): Int {
        val shader = GLES30.glCreateShader(type)
        GLES30.glShaderSource(shader, source)
        GLES30.glCompileShader(shader)
        val compiled = IntArray(1)
        GLES30.glGetShaderiv(shader, GLES30.GL_COMPILE_STATUS, compiled, 0)
        check(compiled[0] != 0) { "compile: " + GLES30.glGetShaderInfoLog(shader) }
        return shader
    }

    private companion object {
        const val TAG = "PadUpscale"

        // A full-screen quad from gl_VertexID alone: no vertex buffer to set up or bind per frame.
        const val QUAD_VS = """#version 310 es
layout(location = 0) out highp vec4 in_TEXCOORD0;
void main() {
    vec2 p = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    in_TEXCOORD0 = vec4(p, 0.0, 0.0);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
"""

        const val COPY_VS = """#version 310 es
uniform mat4 texMatrix;
out highp vec2 uv;
void main() {
    vec2 p = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    uv = (texMatrix * vec4(p, 0.0, 1.0)).xy;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
"""

        const val COPY_FS = """#version 310 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
uniform samplerExternalOES frame;
in highp vec2 uv;
out vec4 color;
void main() {
    color = texture(frame, uv);
}
"""
    }
}
