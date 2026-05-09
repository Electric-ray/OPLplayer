package com.oplplayer.app

import android.content.Context
import android.hardware.usb.*
import com.hoho.android.usbserial.driver.UsbSerialProber
import com.hoho.android.usbserial.driver.UsbSerialPort
import com.hoho.android.usbserial.util.SerialInputOutputManager
import java.util.concurrent.Executors

/**
 * USB OTG 시리얼 관리자
 * adlcom ElectricRay Nuke 3바이트 프로토콜 @ 115200bps
 * 수신 데이터를 OplEngine.nativeFeedSerial()로 전달
 */
class UsbSerialManager(private val context: Context) {

    private var port: UsbSerialPort? = null
    private var ioManager: SerialInputOutputManager? = null
    private val executor = Executors.newSingleThreadExecutor()

    var onStatusChange: ((String) -> Unit)? = null

    /** 연결된 USB 시리얼 장치 목록 반환 */
    fun listDevices(): List<String> {
        val manager = context.getSystemService(Context.USB_SERVICE) as UsbManager
        return UsbSerialProber.getDefaultProber()
            .findAllDrivers(manager)
            .flatMap { driver ->
                driver.ports.mapIndexed { idx, _ ->
                    "${driver.device.productName ?: driver.device.deviceName} [port $idx]"
                }
            }
    }

    /** 첫 번째 사용 가능한 장치에 연결 */
    fun connect(): Boolean {
        val manager = context.getSystemService(Context.USB_SERVICE) as UsbManager
        val drivers  = UsbSerialProber.getDefaultProber().findAllDrivers(manager)
        if (drivers.isEmpty()) {
            onStatusChange?.invoke("USB 시리얼 장치 없음")
            return false
        }
        val driver = drivers.first()
        if (!manager.hasPermission(driver.device)) {
            onStatusChange?.invoke("USB 권한 없음 - AndroidManifest USB 필터 확인")
            return false
        }
        val connection = manager.openDevice(driver.device) ?: run {
            onStatusChange?.invoke("USB 장치 열기 실패")
            return false
        }
        port = driver.ports.first().also { p ->
            p.open(connection)
            p.setParameters(115200, 8, UsbSerialPort.STOPBITS_1, UsbSerialPort.PARITY_NONE)
            p.dtr = true
            p.rts = true
        }
        ioManager = SerialInputOutputManager(port, object : SerialInputOutputManager.Listener {
            override fun onNewData(data: ByteArray) {
                OplEngine.nativeFeedSerial(data, data.size)
            }
            override fun onRunError(e: Exception) {
                onStatusChange?.invoke("USB 오류: ${e.message}")
                disconnect()
            }
        }).also { executor.submit(it) }

        OplEngine.nativeSetSerialEnabled(true)
        onStatusChange?.invoke("USB 시리얼 연결됨: ${driver.device.productName}")
        return true
    }

    fun disconnect() {
        OplEngine.nativeSetSerialEnabled(false)
        ioManager?.stop()
        ioManager = null
        try { port?.close() } catch (_: Exception) {}
        port = null
        onStatusChange?.invoke("USB 시리얼 연결 해제")
    }

    val isConnected get() = port != null
}
