/*
 * tun2socks_jni.c — thin JNI bridge between Kotlin and the native engine.
 */

#include <jni.h>
#include "tun2socks_engine.h"

static engine_t  g_engine;
static JavaVM   *g_jvm = NULL;

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    g_jvm = vm;
    return JNI_VERSION_1_6;
}

JNIEXPORT jint JNICALL
Java_com_throttlevpn_engine_Tun2SocksEngine_nativeStart(
        JNIEnv *env, jobject thiz, jint tun_fd, jint mtu) {
    return (jint)engine_start(&g_engine, (int)tun_fd, (int)mtu,
                               g_jvm, env, thiz);
}

JNIEXPORT void JNICALL
Java_com_throttlevpn_engine_Tun2SocksEngine_nativeStop(
        JNIEnv *env, jobject thiz) {
    (void)thiz;
    engine_stop(&g_engine, env);
}

JNIEXPORT void JNICALL
Java_com_throttlevpn_engine_Tun2SocksEngine_nativeSetRateLimit(
        JNIEnv *env, jobject thiz,
        jlong download_bps, jlong upload_bps) {
    (void)env; (void)thiz;
    engine_set_rates(&g_engine, (int64_t)download_bps, (int64_t)upload_bps);
}

JNIEXPORT jlongArray JNICALL
Java_com_throttlevpn_engine_Tun2SocksEngine_nativeGetStats(
        JNIEnv *env, jobject thiz) {
    (void)thiz;
    int64_t ul, dl;
    engine_get_stats(&g_engine, &ul, &dl);

    jlongArray arr = (*env)->NewLongArray(env, 2);
    jlong vals[2] = { (jlong)ul, (jlong)dl };
    (*env)->SetLongArrayRegion(env, arr, 0, 2, vals);
    return arr;
}
