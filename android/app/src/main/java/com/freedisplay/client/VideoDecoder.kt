package com.freedisplay.client

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.util.Log
import android.view.Surface

/**
 * Hardware H.264 decoder rendering straight to a Surface with minimal buffering.
 * [onDecoded] receives (ptsUs, µs from queueing to decoded output) for each frame.
 */
class VideoDecoder(
    private val surface: Surface,
    private val onStarted: (String) -> Unit,
    private val onDecoded: (Long, Int) -> Unit,
) {
    private val queuedAt = java.util.concurrent.ConcurrentHashMap<Long, Long>()
    private var codec: MediaCodec? = null
    private var outputThread: Thread? = null
    @Volatile private var running = false
    private var width = 0
    private var height = 0

    fun configure(w: Int, h: Int) {
        release()
        width = w
        height = h
    }

    /** Returns false when the decoder needs a keyframe to (re)start. */
    fun feed(data: ByteArray, offset: Int, length: Int, ptsUs: Long, keyframe: Boolean): Boolean {
        var c = codec
        if (c == null) {
            if (!keyframe || width == 0) return false
            c = start(data, offset, length) ?: return false
        }
        return try {
            val index = c.dequeueInputBuffer(50_000)
            if (index < 0) return true // decoder busy; drop this frame
            val buf = c.getInputBuffer(index) ?: return true
            buf.clear()
            buf.put(data, offset, length)
            if (queuedAt.size > 300) queuedAt.clear()
            queuedAt[ptsUs] = System.nanoTime()
            c.queueInputBuffer(index, 0, length, ptsUs, 0)
            true
        } catch (e: Exception) {
            Log.w(TAG, "decode failed", e)
            release()
            false
        }
    }

    private fun start(data: ByteArray, offset: Int, length: Int): MediaCodec? {
        val csd = extractParameterSets(data, offset, length) ?: return null
        val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height).apply {
            setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, width * height)
            setInteger(MediaFormat.KEY_PRIORITY, 0)
            setInteger(MediaFormat.KEY_OPERATING_RATE, Short.MAX_VALUE.toInt())
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            // Vendor switches that stop Qualcomm/MediaTek decoders from holding frames back.
            setInteger("vendor.qti-ext-dec-low-latency.enable", 1)
            setInteger("vendor.qti-ext-dec-picture-order.enable", 1)
            setInteger("vendor.low-latency.enable", 1)
        }
        val c = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
        val lowLatency = Build.VERSION.SDK_INT >= Build.VERSION_CODES.R &&
            c.codecInfo.getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC)
                .isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency)
        try {
            c.configure(format, surface, null, 0)
        } catch (e: Exception) {
            Log.w(TAG, "low-latency configure rejected, retrying plain", e)
            format.setInteger(MediaFormat.KEY_PRIORITY, 1)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) format.setInteger(MediaFormat.KEY_LOW_LATENCY, 0)
            c.configure(format, surface, null, 0)
        }
        c.start()
        // SPS/PPS go in first as codec config.
        val index = c.dequeueInputBuffer(500_000)
        if (index < 0) {
            c.release()
            return null
        }
        c.getInputBuffer(index)!!.apply { clear(); put(csd) }
        c.queueInputBuffer(index, 0, csd.size, 0, MediaCodec.BUFFER_FLAG_CODEC_CONFIG)

        codec = c
        running = true
        outputThread = Thread({ renderLoop(c) }, "decoder-output").also { it.start() }
        Log.i(TAG, "decoder started ${width}x$height (${c.name})")
        onStarted("${c.name}, ${width}x$height, low-latency feature: ${if (lowLatency) "yes" else "no"}")
        return c
    }

    private fun renderLoop(c: MediaCodec) {
        val info = MediaCodec.BufferInfo()
        while (running) {
            val index = try {
                c.dequeueOutputBuffer(info, 20_000)
            } catch (e: Exception) {
                break
            }
            if (index >= 0) {
                val queued = queuedAt.remove(info.presentationTimeUs)
                // Render immediately.
                try { c.releaseOutputBuffer(index, true) } catch (e: Exception) { break }
                if (queued != null) onDecoded(info.presentationTimeUs, ((System.nanoTime() - queued) / 1000).toInt())
            }
        }
    }

    fun release() {
        running = false
        outputThread?.join(500)
        outputThread = null
        codec?.let {
            try { it.stop() } catch (_: Exception) {}
            it.release()
        }
        codec = null
    }

    companion object {
        private const val TAG = "FreeDisplay"

        /** Copies SPS (type 7) and PPS (type 8) NAL units, with start codes, out of an Annex-B frame. */
        fun extractParameterSets(data: ByteArray, offset: Int, length: Int): ByteArray? {
            val end = offset + length
            val starts = ArrayList<Int>()
            var i = offset
            while (i + 3 <= end) {
                if (data[i].toInt() == 0 && data[i + 1].toInt() == 0 &&
                    (data[i + 2].toInt() == 1 || (i + 4 <= end && data[i + 2].toInt() == 0 && data[i + 3].toInt() == 1))
                ) {
                    starts.add(i)
                    i += if (data[i + 2].toInt() == 1) 3 else 4
                } else {
                    i++
                }
            }
            val out = java.io.ByteArrayOutputStream()
            for ((n, s) in starts.withIndex()) {
                val headerLen = if (data[s + 2].toInt() == 1) 3 else 4
                val nalType = data[s + headerLen].toInt() and 0x1f
                val stop = if (n + 1 < starts.size) starts[n + 1] else end
                if (nalType == 7 || nalType == 8) out.write(data, s, stop - s)
            }
            return if (out.size() > 0) out.toByteArray() else null
        }
    }
}
