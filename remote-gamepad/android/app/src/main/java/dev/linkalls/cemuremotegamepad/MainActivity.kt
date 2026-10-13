package dev.linkalls.cemuremotegamepad

import android.app.Activity
import android.graphics.Bitmap
import android.graphics.Color
import android.os.Bundle
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.SurfaceView
import android.widget.FrameLayout
import android.widget.EditText
import android.text.InputType
import android.widget.Button
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import java.io.BufferedInputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.net.InetSocketAddress
import java.net.Socket
import kotlin.math.min
import kotlin.math.roundToInt

/**
 * Cemu GamePad native-frame USB prototype.
 * Run on Windows first: adb reverse tcp:49222 tcp:49222
 * The host sends directly captured RGB24 DRC frames, not desktop screenshots.
 */
class MainActivity : Activity() {
    private lateinit var image: ImageView
    private lateinit var status: TextView
    private lateinit var button: Button
    private lateinit var wifiSurface: SurfaceView
    private lateinit var wifiHost: EditText
    private lateinit var wifiPin: EditText
    private lateinit var wifiToken: EditText
    private lateinit var wifiButton: Button
    private var wifiClient: WifiGamePadClient? = null

    @Volatile private var active = false
    @Volatile private var peer: Socket? = null
    @Volatile private var output: DataOutputStream? = null
    @Volatile private var frameWidth = 854
    @Volatile private var frameHeight = 480
    @Volatile private var generation = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_FULLSCREEN or
            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.BLACK)
        }
        val toolbar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(12, 6, 12, 6)
        }
        status = TextView(this).apply {
            text = "PCで adb reverse tcp:49222 tcp:49222 を実行"
            textSize = 13f
            setTextColor(Color.WHITE)
        }
        button = Button(this).apply {
            text = "接続"
            setOnClickListener { if (active) disconnect() else connect() }
        }
        toolbar.addView(status, LinearLayout.LayoutParams(0, -2, 1f))
        toolbar.addView(button, LinearLayout.LayoutParams(-2, -2))
        root.addView(toolbar)
        val wifiControls = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(8, 4, 8, 4)
        }
        val prefs = getSharedPreferences("cemu-remote-wifi", MODE_PRIVATE)
        wifiHost = EditText(this).apply {
            hint = "PCのIPv4アドレス"
            setSingleLine()
            setText(prefs.getString("host", ""))
            textSize = 12f
            inputType = InputType.TYPE_CLASS_TEXT
        }
        wifiPin = EditText(this).apply {
            hint = "PC証明書SHA256 (64桁)"
            setSingleLine()
            setText(prefs.getString("pin", ""))
            textSize = 12f
            inputType = InputType.TYPE_CLASS_TEXT
        }
        wifiToken = EditText(this).apply {
            hint = "PCトークン（32桁）"
            setSingleLine()
            setText(prefs.getString("token", ""))
            textSize = 12f
            inputType = InputType.TYPE_CLASS_TEXT
        }
        wifiButton = Button(this).apply {
            text = "Wi-Fi TLSで接続"
            setOnClickListener {
                if (wifiClient != null) {
                    wifiClient?.stop()
                    wifiClient = null
                    wifiSurface.visibility = View.VISIBLE
                    image.visibility = View.VISIBLE
                    text = "Wi-Fi TLSで接続"
                } else {
                    val host = wifiHost.text.toString().trim()
                    val pin = wifiPin.text.toString().trim()
                    val token = wifiToken.text.toString().trim()
                    if (!wifiSurface.holder.surface.isValid) {
                        status.text = "Surface未準備。再試行してね"
                        return@setOnClickListener
                    }
                    prefs.edit().putString("host", host).putString("pin", pin).putString("token", token).apply()
                    disconnect()
                    try {
                        wifiSurface.visibility = View.VISIBLE
                        image.visibility = View.GONE
                        val client = WifiGamePadClient(host, pin, token, wifiSurface.holder.surface) {
                            message -> runOnUiThread { status.text = message }
                        }
                        client.start()
                        wifiClient = client
                        text = "Wi-Fi切断"
                    } catch (error: Exception) {
                        wifiSurface.visibility = View.VISIBLE
                        image.visibility = View.VISIBLE
                        status.text = "Wi-Fi設定エラー: " + error.message
                    }
                }
            }
        }
        wifiControls.addView(wifiHost, LinearLayout.LayoutParams(0, -2, 1f))
        wifiControls.addView(wifiPin, LinearLayout.LayoutParams(0, -2, 2f))
        wifiControls.addView(wifiToken, LinearLayout.LayoutParams(0, -2, 1f))
        wifiControls.addView(wifiButton, LinearLayout.LayoutParams(-2, -2))
        root.addView(wifiControls)
        val display = FrameLayout(this)
        image = ImageView(this).apply {
            setBackgroundColor(Color.BLACK)
            scaleType = ImageView.ScaleType.FIT_CENTER
            isClickable = true
            setOnTouchListener { _, motion ->
                sendTouch(motion)
                true
            }
        }
        wifiSurface = SurfaceView(this).apply {
            visibility = View.VISIBLE
            setOnTouchListener { _, event ->
                sendWifiTouch(event)
                true
            }
        }
        display.addView(wifiSurface, FrameLayout.LayoutParams(-1, -1))
        // The USB ImageView covers this surface until Wi-Fi is selected.
        // Keeping the SurfaceView attached avoids invalid Surface on first tap.
        display.addView(image, FrameLayout.LayoutParams(-1, -1))
        root.addView(display, LinearLayout.LayoutParams(-1, 0, 1f))
        setContentView(root)
    }

    private fun connect() {
        if (active) return
        active = true
        val run = ++generation
        status.text = "USB接続中…"
        button.text = "切断"
        Thread({
            var socket: Socket? = null
            try {
                socket = Socket()
                socket.tcpNoDelay = true
                socket.connect(InetSocketAddress("127.0.0.1", 49222), 3000)
                peer = socket
                output = DataOutputStream(socket.getOutputStream())
                val input = DataInputStream(BufferedInputStream(socket.getInputStream(), 131072))
                val header = ByteArray(20)
                runOnUiThread { if (active && run == generation) status.text = "GamePad映像待機中" }

                while (active && run == generation) {
                    input.readFully(header)
                    require(header[0] == 82.toByte() && header[1] == 71.toByte() &&
                        header[2] == 70.toByte() && header[3] == 49.toByte()) { "不明な映像形式" }

                    val w = littleEndianInt(header, 4)
                    val h = littleEndianInt(header, 8)
                    val frameId = littleEndianInt(header, 12)
                    val size = littleEndianInt(header, 16)
                    require(w in 1..1920 && h in 1..1080 && size == w * h * 3) {
                        "不正な映像サイズ"
                    }

                    val rgb = ByteArray(size)
                    input.readFully(rgb)
                    val pixels = IntArray(w * h)
                    var offset = 0
                    for (index in pixels.indices) {
                        val red = rgb[offset++].toInt() and 255
                        val green = rgb[offset++].toInt() and 255
                        val blue = rgb[offset++].toInt() and 255
                        pixels[index] = -16777216 or (red shl 16) or (green shl 8) or blue
                    }
                    val bitmap = Bitmap.createBitmap(pixels, w, h, Bitmap.Config.ARGB_8888)
                    frameWidth = w
                    frameHeight = h
                    runOnUiThread {
                        if (active && run == generation) {
                            image.setImageBitmap(bitmap)
                            status.text = "USB接続中 / " + w + "×" + h + " / #" + frameId
                        } else {
                            bitmap.recycle()
                        }
                    }
                }
            } catch (error: Exception) {
                runOnUiThread {
                    if (run == generation) status.text = "接続失敗: " + (error.message ?: "不明")
                }
            } finally {
                try { socket?.close() } catch (_: Exception) {}
                if (run == generation) {
                    active = false
                    output = null
                    peer = null
                    runOnUiThread { button.text = "接続" }
                }
            }
        }, "Cemu-GamePad-USB").start()
    }

    private fun sendWifiTouch(event: MotionEvent) {
        val down = when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_MOVE -> true
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> false
            else -> return
        }
        val sw = wifiSurface.width.toFloat()
        val sh = wifiSurface.height.toFloat()
        if (sw <= 0f || sh <= 0f) return
        val sourceW = 854f
        val sourceH = 480f
        val scale = min(sw / sourceW, sh / sourceH)
        val drawW = sourceW * scale
        val drawH = sourceH * scale
        val x = ((event.x - (sw - drawW) / 2) / drawW).coerceIn(0f, 1f)
        val y = ((event.y - (sh - drawH) / 2) / drawH).coerceIn(0f, 1f)
        wifiClient?.touch(x, y, down)
    }

    private fun littleEndianInt(data: ByteArray, offset: Int): Int {
        return (data[offset].toInt() and 255) or
            ((data[offset + 1].toInt() and 255) shl 8) or
            ((data[offset + 2].toInt() and 255) shl 16) or
            ((data[offset + 3].toInt() and 255) shl 24)
    }

    private fun sendTouch(event: MotionEvent) {
        val pressed = when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_MOVE -> true
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> false
            else -> return
        }
        val w = frameWidth.toFloat()
        val h = frameHeight.toFloat()
        val vw = image.width.toFloat()
        val vh = image.height.toFloat()
        if (vw <= 0f || vh <= 0f) return

        // The letterboxed regions must not offset the Wii U touch surface.
        val scale = min(vw / w, vh / h)
        val drawnWidth = w * scale
        val drawnHeight = h * scale
        val normalizedX = ((event.x - (vw - drawnWidth) / 2) / drawnWidth).coerceIn(0f, 1f)
        val normalizedY = ((event.y - (vh - drawnHeight) / 2) / drawnHeight).coerceIn(0f, 1f)
        val x = (normalizedX * 65535f).roundToInt()
        val y = (normalizedY * 65535f).roundToInt()
        val stream = output ?: return
        try {
            synchronized(stream) {
                stream.write(byteArrayOf(
                    84, 67, 72, 49,
                    x.toByte(), (x shr 8).toByte(),
                    y.toByte(), (y shr 8).toByte(),
                    if (pressed) 1 else 0
                ))
                stream.flush()
            }
        } catch (_: Exception) {
            // Connection loss is handled on the socket reader thread.
        }
    }

    private fun disconnect() {
        active = false
        generation++
        output = null
        try { peer?.close() } catch (_: Exception) {}
        peer = null
        button.text = "接続"
        status.text = "切断済み"
    }

    override fun onDestroy() {
        wifiClient?.stop()
        wifiClient = null
        disconnect()
        super.onDestroy()
    }
}
