package com.freedisplay.client

import android.app.Activity
import android.content.pm.ActivityInfo
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.view.Gravity
import android.view.MotionEvent
import android.view.OrientationEventListener
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
import kotlin.math.min
import kotlin.math.sin
import kotlin.math.tan

private const val TARGET_HZ = 120f
private const val PREF_ORIENTATION = "orientation"

class MainActivity : Activity(), SurfaceHolder.Callback, StreamClient.Listener {
    private lateinit var root: FrameLayout
    private lateinit var surfaceView: SurfaceView
    private lateinit var status: TextView
    private var client: StreamClient? = null
    private var videoW = 0
    private var videoH = 0
    private var clientPortrait = false // orientation the running client reported to the host
    private var clientRotation = 0     // in-app video rotation the running client uses

    // Orientation setting from the host. Android 16 lets large screens ignore an app's
    // orientation request, so when the screen is the other way round, the app turns
    // the picture itself (decoder rotation) and maps input back.
    private var orientationPref = Protocol.ORIENT_AUTO
    private var rotation = 0                  // 0, 90 or 270 degrees clockwise
    private var heldRotation = 90             // which way the user holds the tablet, from the sensor
    private var orientationListener: OrientationEventListener? = null

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
            setTextColor(Color.rgb(220, 224, 232))
            textSize = 18f
            gravity = Gravity.CENTER
            setLineSpacing(0f, 1.2f)
            setBackgroundColor(Color.BLACK) // covers the last frame after a disconnect
        }
        root.addView(surfaceView, FrameLayout.LayoutParams(-1, -1, Gravity.CENTER))
        root.addView(status, FrameLayout.LayoutParams(-1, -1, Gravity.CENTER))
        root.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ -> fitVideo() }
        setContentView(root)

        refreshRate = requestRefreshRate(TARGET_HZ)
        applyOrientation(getPreferences(MODE_PRIVATE).getInt(PREF_ORIENTATION, Protocol.ORIENT_AUTO))
        orientationListener = object : OrientationEventListener(this) {
            override fun onOrientationChanged(degrees: Int) {
                if (degrees == ORIENTATION_UNKNOWN) return // lying flat: keep the last choice
                // The rotation the system would use for how the tablet is held, versus the
                // one it uses; the difference is what the picture needs (clockwise).
                val held = ((degrees + 45) / 90 % 4) * 90
                val wanted = (360 - held) % 360
                val actual = displayRotationDegrees()
                val extra = (wanted - actual + 360) % 360
                if ((extra == 90 || extra == 270) && extra != heldRotation) {
                    heldRotation = extra
                    // Flip the picture to the new side once the tablet settles (a turn with
                    // auto-rotate on is handled by surfaceChanged instead).
                    surfaceView.removeCallbacks(rotationCheck)
                    surfaceView.postDelayed(rotationCheck, 700)
                }
            }
        }.also { if (it.canDetectOrientation()) it.enable() }
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

    override fun surfaceCreated(holder: SurfaceHolder) = startClient(holder)

    /** Connects with the screen size of the current orientation. */
    private fun startClient(holder: SurfaceHolder) {
        val (w, h) = logicalSize()
        val hz = refreshRate
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            holder.surface.setFrameRate(hz.toFloat(), android.view.Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE)
        }
        clientPortrait = h > w
        clientRotation = rotation
        client = StreamClient(
            holder.surface, w, h, resources.displayMetrics.densityDpi, hz, rotation,
            "${Build.MANUFACTURER} ${Build.MODEL}", this,
        ).also { it.start() }
    }

    private val rotationCheck = Runnable {
        val (w, h) = logicalSize()
        if ((h > w) != clientPortrait || rotation != clientRotation) restartClient()
    }

    private fun restartClient() {
        val holder = surfaceView.holder
        if (client == null || !holder.surface.isValid) return
        client?.stop()
        startClient(holder)
    }

    /**
     * Screen size as the host should see it: the real one, or swapped when the
     * orientation setting wants the other shape (then [rotation] turns the picture).
     */
    private fun logicalSize(): Pair<Int, Int> {
        val (w, h) = screenSize()
        val wantPortrait = orientationPref == Protocol.ORIENT_PORTRAIT
        val mismatch = orientationPref != Protocol.ORIENT_AUTO && (h > w) != wantPortrait
        rotation = if (mismatch) heldRotation else 0
        return if (mismatch) Pair(h, w) else Pair(w, h)
    }

    @Suppress("DEPRECATION")
    private fun displayRotationDegrees(): Int {
        val d = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) display else windowManager.defaultDisplay
        return (d?.rotation ?: 0) * 90
    }

    /** Highest refresh rate we can run at (reported to the host in the hello). */
    private var refreshRate = 60

    /**
     * Asks for the display mode closest to [target] Hz at native resolution and returns
     * its rate. Tablets often idle at 50-60 Hz, which adds up to a frame of delay to
     * every update; in battery saver the host asks for 60 Hz instead.
     */
    @Suppress("DEPRECATION")
    private fun requestRefreshRate(target: Float): Int {
        val display = (if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) display else windowManager.defaultDisplay)
            ?: return refreshRate
        val current = display.mode
        val best = display.supportedModes
            .filter { it.physicalWidth == current.physicalWidth && it.physicalHeight == current.physicalHeight }
            .minByOrNull { kotlin.math.abs(it.refreshRate - target) } ?: return refreshRate
        window.attributes = window.attributes.apply { preferredDisplayModeId = best.modeId }
        return Math.round(best.refreshRate)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        // Rotated: reconnect so the host switches the virtual monitor to the new shape.
        val (w, h) = logicalSize()
        if (client != null && ((h > w) != clientPortrait || rotation != clientRotation)) {
            client?.stop()
            startClient(holder)
        }
    }

    /** Applies the host's orientation setting (also remembered for the next start). */
    private fun applyOrientation(orientation: Int) {
        orientationPref = orientation
        requestedOrientation = when (orientation) {
            Protocol.ORIENT_LANDSCAPE -> ActivityInfo.SCREEN_ORIENTATION_USER_LANDSCAPE
            Protocol.ORIENT_PORTRAIT -> ActivityInfo.SCREEN_ORIENTATION_USER_PORTRAIT
            else -> ActivityInfo.SCREEN_ORIENTATION_FULL_USER // follows auto-rotate and the rotation lock
        }
    }

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
        return Pair(w, h)
    }

    // ---- StreamClient.Listener (called from the network thread) ----

    override fun onStatus(text: String) = runOnUiThread {
        status.text = text
        status.visibility = View.VISIBLE
    }

    override fun onVideoSize(width: Int, height: Int, fps: Int) = runOnUiThread {
        videoW = width
        videoH = height
        fitVideo()
        if (fps > 0) {
            // Match the screen to the stream: 120 Hz normally, 60 Hz in battery saver.
            val hz = requestRefreshRate(fps.toFloat())
            val surface = surfaceView.holder.surface
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && surface.isValid) {
                surface.setFrameRate(hz.toFloat(), android.view.Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE)
            }
        }
    }

    override fun onStreaming() = runOnUiThread { status.visibility = View.GONE }

    override fun onOrientation(orientation: Int) = runOnUiThread {
        getPreferences(MODE_PRIVATE).edit().putInt(PREF_ORIENTATION, orientation).apply()
        if (orientation != orientationPref) {
            applyOrientation(orientation)
            // If the system ignores the request, turn the picture in the app instead.
            val (w, h) = logicalSize()
            if ((h > w) != clientPortrait || rotation != clientRotation) restartClient()
        }
    }

    override fun onDestroy() {
        orientationListener?.disable()
        super.onDestroy()
    }

    /** Letterboxes the SurfaceView to the video's aspect ratio so input maps 1:1. */
    private fun fitVideo() {
        if (videoW == 0 || root.width == 0) return
        val shownW = if (clientRotation == 0) videoW else videoH // the decoder turns the picture
        val shownH = if (clientRotation == 0) videoH else videoW
        val scale = min(root.width / shownW.toFloat(), root.height / shownH.toFloat())
        val lp = surfaceView.layoutParams as FrameLayout.LayoutParams
        val w = (shownW * scale).toInt()
        val h = (shownH * scale).toInt()
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
                val shownTiltX = Math.toDegrees(atan(t * sin(orient)).toDouble()).toFloat()
                val shownTiltY = Math.toDegrees(atan(-t * cos(orient)).toDouble()).toFloat()

                // Map from the shown picture back to the video (undo the in-app rotation).
                val dx = (x / w).coerceIn(0f, 1f)
                val dy = (y / h).coerceIn(0f, 1f)
                val (vx, vy) = when (clientRotation) {
                    90 -> Pair(dy, 1f - dx)
                    270 -> Pair(1f - dy, dx)
                    else -> Pair(dx, dy)
                }
                val (tiltX, tiltY) = when (clientRotation) {
                    90 -> Pair(shownTiltY, -shownTiltX)
                    270 -> Pair(-shownTiltY, shownTiltX)
                    else -> Pair(shownTiltX, shownTiltY)
                }

                writeInt(e.getPointerId(i))
                writeByte(tool)
                writeFloat(vx)
                writeFloat(vy)
                writeFloat(pressure.coerceIn(0f, 1f))
                writeFloat(tiltX)
                writeFloat(tiltY)
                writeFloat(0f)
                writeByte(if (e.buttonState and MotionEvent.BUTTON_STYLUS_PRIMARY != 0) 1 else 0)
            }
        }
    }
}
