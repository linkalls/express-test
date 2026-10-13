package dev.linkalls.cemuremotegamepad

import android.media.MediaCodec
import android.media.MediaFormat
import android.view.Surface
import java.io.Closeable

/**
 * H264 elementary-stream Android decoder. Frames must be complete Annex-B
 * access units (not arbitrary TCP chunks). The remote Cemu host owns framing.
 *
 * The decoder renders directly into a Surface to avoid RGB Bitmaps / GC.
 * The calling thread owns this instance; do not share between connections.
 */
class HardwareH264Decoder(
    surface: Surface,
    width: Int = 854,
    height: Int = 480
) : Closeable {
    private val codec = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
    private val info = MediaCodec.BufferInfo()
    private var ptsUs = 0L

    init {
        try {
            val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height)
            if (android.os.Build.VERSION.SDK_INT >= 30) {
                format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            }
            codec.configure(format, surface, null, 0)
            codec.start()
        } catch (error: Exception) {
            codec.release()
            throw error
        }
    }

    fun pushAccessUnit(bytes: ByteArray): Boolean {
        // Do not block the network worker indefinitely when the decoder is
        // overloaded. Dropping a P-frame requires a future IDR resync.
        val input = codec.dequeueInputBuffer(8_000L)
        if (input < 0) {
            drain()
            return false
        }
        val buffer = codec.getInputBuffer(input) ?: return false
        buffer.clear()
        if (bytes.size > buffer.capacity()) {
            throw IllegalArgumentException("H264 access unit too large: " + bytes.size)
        }
        buffer.put(bytes)
        ptsUs += 16_667L
        codec.queueInputBuffer(input, 0, bytes.size, ptsUs, 0)
        drain()
        return true
    }

    private fun drain() {
        while (true) {
            val output = codec.dequeueOutputBuffer(info, 0)
            when {
                output >= 0 -> codec.releaseOutputBuffer(output, info.size > 0)
                output == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> Unit
                output == MediaCodec.INFO_TRY_AGAIN_LATER -> return
                else -> return
            }
        }
    }

    override fun close() {
        try { codec.stop() } catch (_: Exception) {}
        codec.release()
    }
}
