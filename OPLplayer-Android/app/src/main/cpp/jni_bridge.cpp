// jni_bridge.cpp – Kotlin ↔ C++ 연결
#include <jni.h>
#include "opl_engine.h"

#define JNI_METHOD(ret, name) \
    extern "C" JNIEXPORT ret JNICALL Java_com_oplplayer_app_OplEngine_##name

JNI_METHOD(jboolean, nativeStart)(JNIEnv*, jobject) {
    return OplEngine_Start() ? JNI_TRUE : JNI_FALSE;
}
JNI_METHOD(void, nativeStop)(JNIEnv*, jobject) {
    OplEngine_Stop();
}
JNI_METHOD(void, nativeSetUdpEnabled)(JNIEnv*, jobject, jboolean en) {
    OplEngine_SetUdpEnabled(en == JNI_TRUE);
}
JNI_METHOD(void, nativeSetSerialEnabled)(JNIEnv*, jobject, jboolean en) {
    OplEngine_SetSerialEnabled(en == JNI_TRUE);
}
JNI_METHOD(void, nativeFeedUdpPacket)(JNIEnv* env, jobject, jbyteArray data, jint len) {
    jbyte* bytes = env->GetByteArrayElements(data, nullptr);
    if (bytes) {
        OplEngine_FeedUdpPacket((const uint8_t*)bytes, (int)len);
        env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
    }
}
JNI_METHOD(void, nativeFeedSerial)(JNIEnv* env, jobject, jbyteArray data, jint len) {
    jbyte* bytes = env->GetByteArrayElements(data, nullptr);
    if (bytes) {
        OplEngine_FeedSerial((const uint8_t*)bytes, (int)len);
        env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
    }
}
JNI_METHOD(void, nativeSetVolume)(JNIEnv*, jobject, jfloat v) {
    OplEngine_SetVolume((float)v);
}
JNI_METHOD(void, nativeSetGain)(JNIEnv*, jobject, jfloat g) {
    OplEngine_SetGain((float)g);
}
JNI_METHOD(void, nativeSetPrebufMs)(JNIEnv*, jobject, jint ms) {
    OplEngine_SetPrebufMs((int)ms);
}
JNI_METHOD(void, nativeResetChip)(JNIEnv*, jobject) {
    OplEngine_InitChip();
}
JNI_METHOD(jstring, nativeGetStats)(JNIEnv* env, jobject) {
    return env->NewStringUTF(OplEngine_GetStats().c_str());
}
