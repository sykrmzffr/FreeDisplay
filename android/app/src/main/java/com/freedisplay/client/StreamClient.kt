package com.freedisplay.client

import android.util.Log
import android.view.Surface
import java.io.BufferedInputStream
import java.io.DataInputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

/**
 * Connects to the Windows host through `adb reverse`, decodes its video onto
 * [surface] and sends input messages back. Reconnects until [stop] is called.
 */
class StreamClient(
    private val surface: Surface,
    private val screenWidth: Int,
    private val screenHeight: Int,
    private val dpi: Int,
    private val refreshHz: Int,
    private val rotationDegrees: Int, // video rotation on screen, see MainActivity
    private val deviceName: String,
    private val listener: Listener,
) {
    interface Listener {
        fun onStatus(text: String)
        fun onVideoSize(width: Int, height: Int, fps: Int)
        fun onStreaming()
        fun onOrientation(orientation: Int)
    }

    @Volatile private var running = false
    @Volatile private var socket: Socket? = null
    private var thread: Thread? = null
    private val outbox = LinkedBlockingQueue<ByteArray>(256)

    fun start() {
        running = true
        thread = Thread({ loop() }, "stream-client").also { it.start() }
    }

    fun stop() {
        running = false
        try { socket?.close() } catch (_: Exception) {}
        thread?.join(1000)
        thread = null
    }

    /** Queues a message for the host; drops it if the link is backed up. */
    fun send(message: ByteArray) {
        if (socket != null) outbox.offer(message)
    }

    private fun loop() {
        while (running) {
            listener.onStatus(
                "Tandem\n\nWaiting for your PC…\n\n" +
                    "1.  Run Tandem on Windows (look for its tray icon)\n" +
                    "2.  Connect this tablet with a USB cable\n" +
                    "3.  Allow USB debugging if the tablet asks"
            )
            try {
                Socket().use { s ->
                    s.tcpNoDelay = true
                    s.receiveBufferSize = 4 * 1024 * 1024
                    s.connect(InetSocketAddress("127.0.0.1", Protocol.PORT), 1000)
                    socket = s
                    session(s)
                }
            } catch (e: Exception) {
                if (running) Log.d(TAG, "connection ended: ${e.message}")
            } finally {
                socket = null
                outbox.clear()
            }
            if (running) Thread.sleep(500)
        }
    }

    private fun session(s: Socket) {
        val writer = Thread({
            val out = s.getOutputStream()
            try {
                while (running && !s.isClosed) {
                    val m = outbox.poll(200, TimeUnit.MILLISECONDS) ?: continue
                    out.write(m)
                }
            } catch (_: Exception) {
                try { s.close() } catch (_: Exception) {}
            }
        }, "stream-writer")
        writer.start()

        send(Protocol.message(Protocol.HELLO) {
            writeInt(Protocol.MAGIC)
            writeShort(Protocol.VERSION)
            writeInt(screenWidth)
            writeInt(screenHeight)
            writeInt(dpi)
            writeInt(refreshHz)
            val (maxW, maxH) = VideoDecoder.maxSize(screenWidth, screenHeight)
            writeInt(maxW)
            writeInt(maxH)
            write(deviceName.toByteArray(Charsets.UTF_8))
        })

        val input = DataInputStream(BufferedInputStream(s.getInputStream(), 1 shl 20))
        val decoder = VideoDecoder(
            surface,
            rotationDegrees,
            onStarted = { info ->
                send(Protocol.message(Protocol.DECODER_INFO) { write(info.toByteArray(Charsets.UTF_8)) })
            },
            onDecoded = { pts, decodeUs, waitUs ->
                send(Protocol.message(Protocol.FRAME_STATS) { writeLong(pts); writeInt(decodeUs); writeInt(waitUs) })
            },
        )
        var buffer = ByteArray(1 shl 20)
        var streaming = false
        try {
            while (running) {
                val type = input.readUnsignedByte()
                val length = input.readInt()
                if (length < 0 || length > 64 * 1024 * 1024) throw IllegalStateException("bad length $length")
                if (buffer.size < length) buffer = ByteArray(length + (length shr 2))
                input.readFully(buffer, 0, length)
                val receivedNs = System.nanoTime()
                when (type) {
                    Protocol.PING -> send(Protocol.message(Protocol.PONG) { write(buffer, 0, length) })
                    Protocol.ORIENTATION -> if (length >= 1) listener.onOrientation(buffer[0].toInt())
                    Protocol.VIDEO_CONFIG -> {
                        val w = readInt(buffer, 0)
                        val h = readInt(buffer, 4)
                        val fps = if (length >= 12) readInt(buffer, 8) else 0 // 0: host did not say
                        decoder.configure(w, h)
                        listener.onVideoSize(w, h, fps)
                    }
                    Protocol.VIDEO_FRAME -> {
                        val pts = readLong(buffer, 0)
                        val key = (buffer[8].toInt() and 1) != 0
                        if (!decoder.feed(buffer, 9, length - 9, pts, key, receivedNs)) {
                            send(Protocol.message(Protocol.REQUEST_KEYFRAME))
                        } else if (!streaming) {
                            streaming = true
                            listener.onStreaming()
                        }
                    }
                }
            }
        } finally {
            decoder.release()
            try { s.close() } catch (_: Exception) {}
            writer.join(500)
        }
    }

    private fun readInt(b: ByteArray, o: Int): Int =
        (b[o].toInt() and 0xff shl 24) or (b[o + 1].toInt() and 0xff shl 16) or
            (b[o + 2].toInt() and 0xff shl 8) or (b[o + 3].toInt() and 0xff)

    private fun readLong(b: ByteArray, o: Int): Long =
        (readInt(b, o).toLong() shl 32) or (readInt(b, o + 4).toLong() and 0xffffffffL)

    companion object {
        private const val TAG = "Tandem"
    }
}
