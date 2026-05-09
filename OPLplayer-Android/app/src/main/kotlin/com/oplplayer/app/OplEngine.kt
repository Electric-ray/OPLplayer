package com.oplplayer.app

/** C++ 엔진의 JNI 래퍼 */
object OplEngine {
    init { System.loadLibrary("oplplayer") }

    external fun nativeStart(): Boolean
    external fun nativeStop()
    external fun nativeSetUdpEnabled(en: Boolean)
    external fun nativeSetSerialEnabled(en: Boolean)
    external fun nativeFeedUdpPacket(data: ByteArray, len: Int)  // Kotlin UDP → C++ 파싱
    external fun nativeFeedSerial(data: ByteArray, len: Int)
    external fun nativeSetVolume(v: Float)
    external fun nativeSetGain(g: Float)
    external fun nativeSetPrebufMs(ms: Int)
    external fun nativeResetChip()
    external fun nativeGetStats(): String
}
