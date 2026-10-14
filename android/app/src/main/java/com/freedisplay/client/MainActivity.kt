package com.freedisplay.client

import android.app.Activity
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.view.Gravity
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.TextView
import kotlin.math.atan
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sin
import kotlin.math.tan

private const val TARGET_HZ = 120f

class MainActivity : Activity(), SurfaceHolder.Callback, StreamClient.Listener {
    private lateinit var root: FrameLayout
    private lateinit var surfaceView: SurfaceView
    private lateinit var status: TextView
    private var client: StreamClient? = null
    private var videoW = 0
    private var videoH = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            window.attributes.layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
        }

        root = FrameLayout(this).apply { setBackgroundColor(Color.BLACK) }
        surfaceView = SurfaceView(this)
        status = TextView(this).apply {
            setTextColor(Color.WHITE)
            textSize = 20f
            gravity = Gravity.CENTER
        }
        root.addView(surfaceView, FrameLayout.LayoutParams(-1, -1, Gravity.CENTER))
        root.addView(status, FrameLayout.LayoutParams(-1, -1, Gravity.CENTER))
        root.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ -> fitVideo() }
        setContentView(root)

        requestHighRefreshRate()
        surfaceView.holder.addCallback(this)
        surfaceView.setOnTouchListener { _, e -> sendMotion(e); true }
        surfaceView.setOnGenericMotionListener { _, e -> sendMotion(e); true }
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) hideSystemBars()
    }

    @Suppress("DEPRECATION")
    private fun hideSystemBars() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.insetsController?.let {
                it.hide(WindowInsets.Type.systemBars())
                it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
                View.SYSTEM_UI_FLAG_FULLSCREEN or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
        }
    }

    // ---- surface lifecycle drives the connection ----

    override fun surfaceCreated(holder: SurfaceHolder) {
        val (w, h) = screenSize()
        val hz = refreshRate
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            holder.surface.setFrameRate(hz.toFloat(), android.view.Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE)
        }
        client = StreamClient(
            holder.surface, w, h, resources.displayMetrics.densityDpi, hz,
            "${Build.MANUFACTURER} ${Build.MODEL}", this,
        ).also { it.start() }
    }

    /** Refresh rate of the display mode we asked for in [requestHighRefreshRate]. */
    private var refreshRate = 60

    /**
     * Asks for the display mode closest to 120 Hz at native resolution. Tablets often
     * idle at 50-60 Hz, which adds up to a frame of delay to every update.
     */
    @Suppress("DEPRECATION")
    private fun requestHighRefreshRate() {
        val display = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) display else windowManager.defaultDisplay
        display ?: return
        val current = display.mode
        val best = display.supportedModes
            .filter { it.physicalWidth == current.physicalWidth && it.physicalHeight == current.physicalHeight }
            .minByOrNull { kotlin.math.abs(it.refreshRate - TARGET_HZ) } ?: return
        window.attributes = window.attributes.apply { preferredDisplayModeId = best.modeId }
        refreshRate = Math.round(best.refreshRate)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        client?.stop()
        client = null
    }

    @Suppress("DEPRECATION")
    private fun screenSize(): Pair<Int, Int> {
        val w: Int
        val h: Int
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val b = windowManager.maximumWindowMetrics.bounds
            w = b.width(); h = b.height()
        } else {
            val m = android.util.DisplayMetrics()
            windowManager.defaultDisplay.getRealMetrics(m)
            w = m.widthPixels; h = m.heightPixels
        }
        return Pair(max(w, h), min(w, h)) // always report landscape
    }

    // ---- StreamClient.Listener (called from the network thread) ----

    override fun onStatus(text: String) = runOnUiThread {
        status.text = text
        status.visibility = View.VISIBLE
    }

    override fun onVideoSize(width: Int, height: Int) = runOnUiThread {
        videoW = width
        videoH = height
        fitVideo()
    }

    override fun onStreaming() = runOnUiThread { status.visibility = View.GONE }

    /** Letterboxes the SurfaceView to the video's aspect ratio so input maps 1:1. */
    private fun fitVideo() {
        if (videoW == 0 || root.width == 0) return
        val scale = min(root.width / videoW.toFloat(), root.height / videoH.toFloat())
        val lp = surfaceView.layoutParams as FrameLayout.LayoutParams
        val w = (videoW * scale).toInt()
        val h = (videoH * scale).toInt()
        if (lp.width != w || lp.height != h) {
            lp.width = w
            lp.height = h
            lp.gravity = Gravity.CENTER
            surfaceView.layoutParams = lp
        }
    }

    // ---- input ----

    private fun sendMotion(e: MotionEvent) {
        val c = client ?: return
        val action = when (e.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> Protocol.ACT_DOWN
            MotionEvent.ACTION_MOVE -> Protocol.ACT_MOVE
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> Protocol.ACT_UP
            MotionEvent.ACTION_CANCEL -> Protocol.ACT_CANCEL
            MotionEvent.ACTION_HOVER_ENTER, MotionEvent.ACTION_HOVER_MOVE -> Protocol.ACT_HOVER_MOVE
            MotionEvent.ACTION_HOVER_EXIT -> Protocol.ACT_HOVER_EXIT
            else -> return
        }
        if (e.getToolType(e.actionIndex) == MotionEvent.TOOL_TYPE_MOUSE) return

        // Moves carry batched historical samples; replay them for smooth pen strokes.
        if (action == Protocol.ACT_MOVE || action == Protocol.ACT_HOVER_MOVE) {
            for (h in 0 until e.historySize) c.send(encode(e, action, h))
        }
        c.send(encode(e, action, -1))
    }

    /** Encodes one MotionEvent sample; [hist] selects a historical sample, -1 for current. */
    private fun encode(e: MotionEvent, action: Int, hist: Int): ByteArray {
        val w = surfaceView.width.toFloat().coerceAtLeast(1f)
        val h = surfaceView.height.toFloat().coerceAtLeast(1f)
        return Protocol.message(Protocol.INPUT) {
            writeByte(action)
            writeByte(e.actionIndex)
            writeByte(e.pointerCount)
            for (i in 0 until e.pointerCount) {
                val x = if (hist < 0) e.getX(i) else e.getHistoricalX(i, hist)
                val y = if (hist < 0) e.getY(i) else e.getHistoricalY(i, hist)
                val pressure = if (hist < 0) e.getPressure(i) else e.getHistoricalPressure(i, hist)
                val tilt = if (hist < 0) e.getAxisValue(MotionEvent.AXIS_TILT, i)
                           else e.getHistoricalAxisValue(MotionEvent.AXIS_TILT, i, hist)
                val orient = if (hist < 0) e.getOrientation(i) else e.getHistoricalOrientation(i, hist)
                val tool = when (e.getToolType(i)) {
                    MotionEvent.TOOL_TYPE_STYLUS -> Protocol.TOOL_PEN
                    MotionEvent.TOOL_TYPE_ERASER -> Protocol.TOOL_ERASER
                    else -> Protocol.TOOL_FINGER
                }
                // Android gives tilt from vertical plus a direction; Windows wants X/Y tilt.
                val t = tan(tilt.coerceIn(0f, 1.5f))
                val tiltX = Math.toDegrees(atan(t * sin(orient)).toDouble()).toFloat()
                val tiltY = Math.toDegrees(atan(-t * cos(orient)).toDouble()).toFloat()

                writeInt(e.getPointerId(i))
                writeByte(tool)
                writeFloat((x / w).coerceIn(0f, 1f))
                writeFloat((y / h).coerceIn(0f, 1f))
                writeFloat(pressure.coerceIn(0f, 1f))
                writeFloat(tiltX)
                writeFloat(tiltY)
                writeFloat(0f)
                writeByte(if (e.buttonState and MotionEvent.BUTTON_STYLUS_PRIMARY != 0) 1 else 0)
            }
        }
    }
}
