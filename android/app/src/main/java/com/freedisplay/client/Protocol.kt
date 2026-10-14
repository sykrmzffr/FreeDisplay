package com.freedisplay.client

import java.io.ByteArrayOutputStream
import java.io.DataOutputStream

/** Wire protocol shared with the Windows host. See PROTOCOL.md. */
object Protocol {
    const val PORT = 27184
    const val MAGIC = 0x46445350 // "FDSP"
    const val VERSION = 2

    const val HELLO = 1
    const val VIDEO_CONFIG = 2
    const val VIDEO_FRAME = 3
    const val INPUT = 4
    const val REQUEST_KEYFRAME = 6
    const val FRAME_STATS = 7
    const val DECODER_INFO = 8

    const val ACT_DOWN = 0
    const val ACT_MOVE = 1
    const val ACT_UP = 2
    const val ACT_HOVER_MOVE = 3
    const val ACT_HOVER_EXIT = 4
    const val ACT_CANCEL = 5

    const val TOOL_FINGER = 0
    const val TOOL_PEN = 1
    const val TOOL_ERASER = 2

    /** Frames one message: u8 type, u32 length, payload. */
    fun message(type: Int, build: DataOutputStream.() -> Unit = {}): ByteArray {
        val body = ByteArrayOutputStream()
        DataOutputStream(body).build()
        val payload = body.toByteArray()
        val out = ByteArrayOutputStream(payload.size + 5)
        DataOutputStream(out).apply {
            writeByte(type)
            writeInt(payload.size)
            write(payload)
        }
        return out.toByteArray()
    }
}
