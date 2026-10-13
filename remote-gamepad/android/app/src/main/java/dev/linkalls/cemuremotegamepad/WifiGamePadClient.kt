package dev.linkalls.cemuremotegamepad

import android.view.Surface
import java.io.BufferedInputStream
import java.io.DataInputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.security.MessageDigest
import java.security.SecureRandom
import java.security.cert.X509Certificate
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLSocket
import javax.net.ssl.TrustManager
import javax.net.ssl.X509TrustManager

/**
 * Experimental encrypted Wi-Fi GamePad client.
 *
 * A 64-digit SHA256 certificate fingerprint MUST be entered from the PC's
 * %LOCALAPPDATA%/CemuRemoteGamePad/wifi-fingerprint.txt before connecting.
 * The trust manager accepts the self-signed handshake certificate only so
 * that it can be compared with the pre-shared fingerprint BEFORE sending any
 * GamePad touch commands or trusting received video.
 *
 * The host uses TLS 1.3 on port 49224 and emits framed Annex-B AVC units.
 */
class WifiGamePadClient(
    private val host: String,
    fingerprint: String,
    private val surface: Surface,
    private val onState: (String) -> Unit
) {
    private val expectedPin = fingerprint.filter { it.isLetterOrDigit() }.uppercase()
    private var worker: Thread? = null
    @Volatile private var connected = false
    @Volatile private var socket: SSLSocket? = null
    @Volatile private var output: OutputStream? = null

    fun start() {
        require(expectedPin.matches(Regex("[0-9A-F]{64}"))) {
            "PCの証明書SHA256フィンガープリント（64桁）が必要"
        }
        require(host.isNotBlank()) { "PCのIPアドレスを入力してね" }
        if (worker != null) return

        worker = Thread({
            var decoder: HardwareH264Decoder? = null
            try {
                onState("Wi-Fi TLS接続中…")
                val raw = Socket()
                raw.tcpNoDelay = true
                raw.connect(InetSocketAddress(host, 49224), 4000)

                // The only trust anchor is the 64-character, manually verified
                // fingerprint supplied through the UI. No TOFU / auto-accept.
                val trustManager = object : X509TrustManager {
                    override fun checkClientTrusted(chain: Array<X509Certificate>, authType: String) = Unit
                    override fun checkServerTrusted(chain: Array<X509Certificate>, authType: String) = Unit
                    override fun getAcceptedIssuers(): Array<X509Certificate> = emptyArray()
                }
                val context = SSLContext.getInstance("TLSv1.3")
                context.init(null, arrayOf<TrustManager>(trustManager), SecureRandom())
                val tls = context.socketFactory.createSocket(raw, host, 49224, true) as SSLSocket
                socket = tls
                tls.enabledProtocols = arrayOf("TLSv1.3")
                tls.startHandshake()

                val certificate = tls.session.peerCertificates.firstOrNull() ?: error("PC証明書がない")
                val calculated = MessageDigest.getInstance("SHA-256")
                    .digest(certificate.encoded).joinToString("") { "%02X".format(it) }
                if (calculated != expectedPin) {
                    error("PCの証明書が一致しません。接続を拒否しました")
                }

                output = tls.outputStream
                connected = true
                onState("暗号化されたWi-Fi接続：映像待機中")
                val input = DataInputStream(BufferedInputStream(tls.inputStream, 128 * 1024))
                val header = ByteArray(20)
                while (connected) {
                    input.readFully(header)
                    require(header[0] == 65.toByte() && header[1] == 86.toByte() &&
                        header[2] == 67.toByte() && header[3] == 49.toByte()) {
                        "未対応の映像プロトコル"
                    }
                    val width = getLE32(header, 4)
                    val height = getLE32(header, 8)
                    val sequence = getLE32(header, 12)
                    val size = getLE32(header, 16)
                    require(width in 2..1920 && height in 2..1080 && size in 1..2_097_152) {
                        "不正な映像ヘッダー"
                    }
                    val accessUnit = ByteArray(size)
                    input.readFully(accessUnit)
                    if (decoder == null) {
                        decoder = HardwareH264Decoder(surface, width, height)
                    }
                    if (!decoder.pushAccessUnit(accessUnit)) {
                        onState("映像デコーダー混雑：フレーム破棄")
                    } else if (sequence % 60 == 0) {
                        onState("Wi-Fi / H.264 / " + width + "×" + height +
                            " / パケット #" + sequence)
                    }
                }
            } catch (error: Exception) {
                onState("Wi-Fi接続エラー: " + (error.message ?: error.javaClass.simpleName))
            } finally {
                connected = false
                output = null
                try { decoder?.close() } catch (_: Exception) {}
                try { socket?.close() } catch (_: Exception) {}
                socket = null
                worker = null
            }
        }, "Cemu-H264-TLS")
        worker?.start()
    }

    fun touch(x: Float, y: Float, pressed: Boolean) {
        if (!connected) return
        val xs = (x.coerceIn(0f, 1f) * 65535).toInt()
        val ys = (y.coerceIn(0f, 1f) * 65535).toInt()
        val packet = byteArrayOf(
            84, 67, 72, 49,
            xs.toByte(), (xs shr 8).toByte(),
            ys.toByte(), (ys shr 8).toByte(),
            if (pressed) 1 else 0
        )
        try {
            val out = output ?: return
            synchronized(out) {
                out.write(packet)
                out.flush()
            }
        } catch (_: Exception) {
            stop()
        }
    }

    fun stop() {
        connected = false
        try { socket?.close() } catch (_: Exception) {}
        socket = null
        output = null
    }

    private fun getLE32(a: ByteArray, i: Int): Int {
        return (a[i].toInt() and 255) or
            ((a[i + 1].toInt() and 255) shl 8) or
            ((a[i + 2].toInt() and 255) shl 16) or
            ((a[i + 3].toInt() and 255) shl 24)
    }
}
