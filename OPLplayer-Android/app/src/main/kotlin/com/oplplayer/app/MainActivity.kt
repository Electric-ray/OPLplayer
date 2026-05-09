package com.oplplayer.app

import android.content.Context
import android.net.wifi.WifiManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.widget.*
import androidx.appcompat.app.AppCompatActivity

class MainActivity : AppCompatActivity() {

    private lateinit var btnUdp:        Button
    private lateinit var btnUsb:        Button
    private lateinit var btnReset:      Button
    private lateinit var sliderVol:     SeekBar
    private lateinit var sliderGain:    SeekBar
    private lateinit var tvStats:       TextView
    private lateinit var tvLog:         TextView
    private lateinit var spinnerPrebuf: Spinner
    private lateinit var ledIndicator:  android.view.View

    private val usbManager by lazy { UsbSerialManager(this) }
    private val udpReceiver by lazy { UdpReceiver(this) }

    private var udpOn = false
    private var usbOn = false

    // WiFi 절전 방지 락
    private var wifiLock:      WifiManager.WifiLock?      = null
    private var multicastLock: WifiManager.MulticastLock? = null

    // LED 점멸용 이전 카운트
    private var prevUdpPkt  = 0L
    private var prevComEvt  = 0L
    private var ledOn       = false

    private val handler = Handler(Looper.getMainLooper())
    private val statsRunnable = object : Runnable {
        override fun run() {
            val stats = OplEngine.nativeGetStats()
            tvStats.text = stats
            updateLed(stats)
            handler.postDelayed(this, 300)
        }
    }

    private fun updateLed(stats: String) {
        // "UDP pkt:N" 에서 N 파싱
        val udpPkt = parseAfter(stats, "pkt:")
        val comEvt = parseAfter(stats, "Serial ev:")
        val active = (udpOn && udpPkt > prevUdpPkt) ||
                     (usbOn && comEvt > prevComEvt)
        if (active != ledOn) {
            ledOn = active
            ledIndicator.setBackgroundResource(
                if (active) R.drawable.led_active else R.drawable.led_inactive
            )
        }
        prevUdpPkt = udpPkt
        prevComEvt = comEvt
    }

    private fun parseAfter(s: String, key: String): Long {
        val idx = s.indexOf(key)
        if (idx < 0) return 0L
        val start = idx + key.length
        val end = s.indexOfFirst { it == ' ' || it == '|' || it == '\n' }.let { i ->
            val sub = s.substring(start)
            val j = sub.indexOfFirst { !it.isDigit() }
            if (j < 0) s.length else start + j
        }
        return s.substring(start, end).toLongOrNull() ?: 0L
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        btnUdp        = findViewById(R.id.btnUdp)
        btnUsb        = findViewById(R.id.btnUsb)
        btnReset      = findViewById(R.id.btnReset)
        sliderVol     = findViewById(R.id.sliderVol)
        sliderGain    = findViewById(R.id.sliderGain)
        tvStats       = findViewById(R.id.tvStats)
        tvLog         = findViewById(R.id.tvLog)
        spinnerPrebuf = findViewById(R.id.spinnerPrebuf)
        ledIndicator  = findViewById(R.id.ledIndicator)

        // WiFi 락 준비
        val wm = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        wifiLock      = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "OPLplayer:wifi")
        multicastLock = wm.createMulticastLock("OPLplayer:multicast")

        // UDP 수신기 상태 콜백
        udpReceiver.onStatusChange = { msg -> runOnUiThread { log(msg) } }
        usbManager.onStatusChange  = { msg -> runOnUiThread { log(msg) } }

        // 프리버퍼 프리셋
        val presets  = arrayOf("30ms (저지연)", "50ms (권장)", "80ms (안정)", "120ms (느린WiFi)")
        val presetMs = intArrayOf(30, 50, 80, 120)
        spinnerPrebuf.adapter =
            ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, presets)
        spinnerPrebuf.setSelection(1)
        spinnerPrebuf.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(p: AdapterView<*>?, v: android.view.View?, pos: Int, id: Long) {
                OplEngine.nativeSetPrebufMs(presetMs[pos])
            }
            override fun onNothingSelected(p: AdapterView<*>?) {}
        }

        sliderVol.max = 100; sliderVol.progress = 100
        sliderVol.setOnSeekBarChangeListener(simpleSeekBar { OplEngine.nativeSetVolume(it / 100f) })

        sliderGain.max = 300; sliderGain.progress = 100
        sliderGain.setOnSeekBarChangeListener(simpleSeekBar {
            OplEngine.nativeSetGain(0.5f + it * 3.5f / 300f)
        })

        // UDP 버튼
        btnUdp.setOnClickListener {
            udpOn = !udpOn
            if (udpOn) {
                if (usbOn) { usbManager.disconnect(); usbOn = false; btnUsb.text = "USB OPL3 ON" }
                acquireWifiLocks()
                OplEngine.nativeSetUdpEnabled(true)
                udpReceiver.start()
                btnUdp.text = "UDP OPL3 OFF"
                log("UDP 수신 시작 → ESP32 ${udpReceiver.esp32Ip}:${udpReceiver.port}")
            } else {
                udpReceiver.stop()
                OplEngine.nativeSetUdpEnabled(false)
                releaseWifiLocks()
                btnUdp.text = "UDP OPL3 ON"
                log("UDP 수신 중지")
            }
        }

        // USB 버튼
        btnUsb.setOnClickListener {
            if (usbOn) {
                usbManager.disconnect(); usbOn = false
                btnUsb.text = "USB OPL3 ON"
            } else {
                if (udpOn) {
                    udpReceiver.stop()
                    OplEngine.nativeSetUdpEnabled(false)
                    releaseWifiLocks()
                    udpOn = false; btnUdp.text = "UDP OPL3 ON"
                }
                usbOn = usbManager.connect()
                btnUsb.text = if (usbOn) "USB OPL3 OFF" else "USB OPL3 ON"
            }
        }

        btnReset.setOnClickListener {
            OplEngine.nativeResetChip()
            log("OPL3 칩 초기화")
        }

        if (OplEngine.nativeStart()) log("OPLplayer 엔진 시작")
        else log("엔진 시작 실패")
    }

    private fun acquireWifiLocks() {
        if (wifiLock?.isHeld == false)      wifiLock?.acquire()
        if (multicastLock?.isHeld == false) multicastLock?.acquire()
        log("WiFi 락 획득")
    }

    private fun releaseWifiLocks() {
        runCatching { if (wifiLock?.isHeld == true)      wifiLock?.release() }
        runCatching { if (multicastLock?.isHeld == true) multicastLock?.release() }
    }

    override fun onResume()  { super.onResume();  handler.post(statsRunnable) }
    override fun onPause()   { super.onPause();   handler.removeCallbacks(statsRunnable) }
    override fun onDestroy() {
        udpReceiver.stop()
        usbManager.disconnect()
        OplEngine.nativeStop()
        releaseWifiLocks()
        super.onDestroy()
    }

    private fun log(msg: String) {
        tvLog.append("$msg\n")
        val lines = tvLog.text.lines()
        if (lines.size > 30) tvLog.text = lines.takeLast(30).joinToString("\n")
    }

    private fun simpleSeekBar(onChange: (Int) -> Unit) =
        object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(sb: SeekBar, v: Int, fromUser: Boolean) { onChange(v) }
            override fun onStartTrackingTouch(sb: SeekBar) {}
            override fun onStopTrackingTouch(sb: SeekBar) {}
        }
}
