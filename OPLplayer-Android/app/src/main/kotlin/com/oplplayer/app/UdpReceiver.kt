package com.oplplayer.app

import android.content.Context
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.util.Log
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.SocketTimeoutException

/**
 * Kotlin UDP 수신기
 * - ConnectivityManager로 WiFi 네트워크에 명시적 바인딩
 * - 수신 패킷을 OplEngine.nativeFeedUdpPacket()으로 전달
 * - HELLO 패킷으로 ESP32에 자신의 주소 알림
 */
class UdpReceiver(private val context: Context) {

    private var thread: Thread? = null
    @Volatile private var running = false
    @Volatile private var socket: DatagramSocket? = null

    var esp32Ip: String = "192.168.4.1"   // ESP32 AP 기본 IP
    var port:    Int    = 9800

    var onStatusChange: ((String) -> Unit)? = null

    fun start() {
        if (running) return
        running = true
        thread = Thread({
            try {
                val sock = DatagramSocket(null)
                sock.reuseAddress = true
                sock.bind(InetSocketAddress(port))
                sock.soTimeout = 100  // 100ms 타임아웃으로 running 체크
                socket = sock

                // WiFi 네트워크에 소켓 바인딩 (모바일 데이터 우회)
                bindToWifi(sock)

                onStatusChange?.invoke("UDP 포트 $port 수신 대기 중...")
                Log.i("UdpReceiver", "Socket bound to port $port")

                val esp32Addr = InetSocketAddress(InetAddress.getByName(esp32Ip), port)
                val helloData = "HELLO".toByteArray(Charsets.US_ASCII)
                var lastHelloMs = 0L
                var gotAnyPacket = false
                val buf = ByteArray(4096)

                while (running) {
                    // HELLO 패킷 전송 (ESP32가 이 앱의 IP를 알게 함)
                    val nowMs = System.currentTimeMillis()
                    if (!gotAnyPacket && nowMs - lastHelloMs > 300) {
                        try {
                            sock.send(DatagramPacket(helloData, helloData.size, esp32Addr))
                            lastHelloMs = nowMs
                        } catch (_: Exception) {}
                    }

                    try {
                        val pkt = DatagramPacket(buf, buf.size)
                        sock.receive(pkt)
                        if (!gotAnyPacket) {
                            gotAnyPacket = true
                            onStatusChange?.invoke("UDP 첫 패킷 수신! (${pkt.address.hostAddress})")
                            Log.i("UdpReceiver", "First packet from ${pkt.address.hostAddress}")
                        }
                        // raw 바이트를 C++ 엔진으로 전달
                        OplEngine.nativeFeedUdpPacket(buf, pkt.length)

                    } catch (_: SocketTimeoutException) {
                        // 정상 타임아웃 - running 체크 후 계속
                    } catch (e: Exception) {
                        if (running) {
                            onStatusChange?.invoke("UDP 오류: ${e.message}")
                            Log.e("UdpReceiver", "receive error", e)
                        }
                        break
                    }
                }
            } catch (e: Exception) {
                onStatusChange?.invoke("UDP 시작 실패: ${e.message}")
                Log.e("UdpReceiver", "start error", e)
            } finally {
                socket?.close()
                socket = null
                Log.i("UdpReceiver", "Thread exit")
            }
        }, "UdpReceiver")
        thread!!.isDaemon = true
        thread!!.start()
    }

    fun stop() {
        running = false
        socket?.close()
        thread?.join(1000)
        thread = null
    }

    /** WiFi 네트워크에 소켓 바인딩 - 모바일 데이터가 있어도 WiFi로 강제 */
    private fun bindToWifi(sock: DatagramSocket) {
        try {
            val cm = context.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
            val wifiNet = cm.allNetworks.firstOrNull { net ->
                cm.getNetworkCapabilities(net)
                    ?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) == true
            }
            if (wifiNet != null) {
                wifiNet.bindSocket(sock)
                val lp = cm.getLinkProperties(wifiNet)
                val ip = lp?.linkAddresses?.firstOrNull()?.address?.hostAddress ?: "?"
                onStatusChange?.invoke("WiFi 네트워크 바인딩 완료 (내 IP: $ip)")
                Log.i("UdpReceiver", "Bound to WiFi, local IP: $ip")
            } else {
                onStatusChange?.invoke("경고: WiFi 네트워크를 찾을 수 없음 - ESP32와 같은 WiFi인지 확인")
                Log.w("UdpReceiver", "No WiFi network found")
            }
        } catch (e: Exception) {
            Log.w("UdpReceiver", "bindToWifi failed: ${e.message}")
        }
    }

    val isRunning get() = running
}
