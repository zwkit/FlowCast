/*
 * JNI bridge between Kotlin NativeBridge and the C RAOP library.
 */

#include <jni.h>
#include <string.h>
#include <stdlib.h>
#include <memory>
#include <string>
#include <android/log.h>

extern "C" {
#include "raop.h"
#include "dnssd.h"
#include "logger.h"
#include "pairing.h"
#include "android_raop_callbacks.h"
#include "android_dnssd_shim.h"
}

#include "audio_engine.h"
#include "log_sink.h"

#define TAG "AirPlayNative"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* Holds all native state for one server instance */
typedef struct {
    raop_t *raop;
    dnssd_t *dnssd;
    android_callback_ctx_t cb_ctx;
    raop_callbacks_t callbacks;
    char hw_addr[6];
    std::string lang_requested, lang_subtitles, lang_system;
    std::shared_ptr<LogSink> log;
} server_ctx_t;

static void _log_callback(void *cls, int level, const char *msg) {
    int prio = ANDROID_LOG_DEBUG;
    if (level >= 5) prio = ANDROID_LOG_ERROR;
    else if (level >= 4) prio = ANDROID_LOG_WARN;
    else if (level >= 3) prio = ANDROID_LOG_INFO;
    __android_log_print(prio, TAG, "%s", msg);
}

extern "C"
JNIEXPORT jlong JNICALL
Java_io_github_jqssun_airplay_bridge_NativeBridge_nativeInit(
        JNIEnv *env, jobject thiz,
        jobject callback, jbyteArray hwAddr, jstring name, jstring keyFile,
        jboolean nohold, jboolean requirePin, jstring pairingId) {

    server_ctx_t *ctx = new server_ctx_t();

    jsize hw_len = env->GetArrayLength(hwAddr);
    if (hw_len > 6) hw_len = 6;
    env->GetByteArrayRegion(hwAddr, 0, hw_len, (jbyte *)ctx->hw_addr);

    android_callbacks_init(&ctx->cb_ctx, env, callback);
    ctx->cb_ctx.require_pin = requirePin ? 1 : 0;
    android_callbacks_fill(&ctx->callbacks, &ctx->cb_ctx);

    ntp_global_init();
    ctx->raop = raop_init(&ctx->callbacks);
    if (!ctx->raop) {
        LOGE("raop_init failed");
        android_callbacks_destroy(&ctx->cb_ctx, env);
        delete ctx;
        return 0;
    }
    ctx->cb_ctx.raop = ctx->raop;

    raop_set_log_level(ctx->raop, LOGGER_ERR);
    raop_set_log_callback(ctx->raop, _log_callback, NULL);

    const char *keyfile_c = env->GetStringUTFChars(keyFile, NULL);
    const char *name_c = env->GetStringUTFChars(name, NULL);

    char device_id[18];
    snprintf(device_id, sizeof(device_id), "%02X:%02X:%02X:%02X:%02X:%02X",
             (unsigned char)ctx->hw_addr[0], (unsigned char)ctx->hw_addr[1],
             (unsigned char)ctx->hw_addr[2], (unsigned char)ctx->hw_addr[3],
             (unsigned char)ctx->hw_addr[4], (unsigned char)ctx->hw_addr[5]);

    int ret = raop_init2(ctx->raop, nohold ? 1 : 0, device_id, keyfile_c);
    if (ret < 0) {
        LOGE("raop_init2 failed: %d", ret);
        env->ReleaseStringUTFChars(keyFile, keyfile_c);
        env->ReleaseStringUTFChars(name, name_c);
        raop_destroy(ctx->raop);
        android_callbacks_destroy(&ctx->cb_ctx, env);
        delete ctx;
        return 0;
    }

    if (requirePin) {
        /* avoid UxPlay's random-PIN retry path: use one random PIN for this server run */
        int pin = random_pin();
        if (pin < 0) {
            LOGE("Failed to generate random pin");
            pin = 1234;
        }
        raop_set_plist(ctx->raop, "pin", pin + 10000);
    }

    /* shim dnssd_init only builds txt records; kotlin does the actual nsd registration */
    int dns_err = 0;
    unsigned char pin_pw = requirePin ? 1 : 0;
    ctx->dnssd = dnssd_init(name_c, (int)strlen(name_c), ctx->hw_addr, 6, pin_pw, &dns_err);
    if (!ctx->dnssd) {
        LOGE("dnssd_init failed: %d", dns_err);
    } else {
        raop_set_dnssd(ctx->raop, ctx->dnssd);
        /* per-install AirPlay pairing identity. without it every receiver on the LAN
         * advertises UxPlay's hardcoded "pi" and iOS treats them as one accessory,
         * connecting to whichever of them it resolved last */
        if (pairingId) {
            const char *pi_c = env->GetStringUTFChars(pairingId, NULL);
            if (pi_c) {
                dnssd_set_pairing_id(ctx->dnssd, pi_c);
                env->ReleaseStringUTFChars(pairingId, pi_c);
            }
        }
    }

    env->ReleaseStringUTFChars(keyFile, keyfile_c);
    env->ReleaseStringUTFChars(name, name_c);

    ctx->log = std::make_shared<LogSink>();
    ctx->log->bind(env, callback);

    ctx->cb_ctx.audio_engine = audio_engine_create(ctx->log, 44100, 2);
    if (!ctx->cb_ctx.audio_engine) {
        LOGE("audio_engine_create failed");
        if (ctx->dnssd) {
            raop_set_dnssd(ctx->raop, NULL);
            dnssd_destroy(ctx->dnssd);
            ctx->dnssd = NULL;
        }
        raop_destroy(ctx->raop);
        android_callbacks_destroy(&ctx->cb_ctx, env);
        delete ctx;
        return 0;
    }

    return (jlong)(intptr_t)ctx;
}

extern "C"
JNIEXPORT jint JNICALL
Java_io_github_jqssun_airplay_bridge_NativeBridge_nativeStart(
        JNIEnv *env, jobject thiz, jlong handle, jint requestedPort) {

    server_ctx_t *ctx = (server_ctx_t *)(intptr_t)handle;
    if (!ctx || !ctx->raop) return -1;

    unsigned short port = (unsigned short)(requestedPort > 0 ? requestedPort : 7000);
    int ret = raop_start_httpd(ctx->raop, &port);
    if (ret < 0) {
        LOGE("raop_start_httpd failed: %d", ret);
        return -1;
    }
    /* raop_start_httpd doesn't record the bound port; without this the hls
       proxy urls are built with port 0 */
    raop_set_port(ctx->raop, port);

    LOGI("AirPlay server started on port %d", port);

    /* Register dnssd records (stored in shim, Kotlin reads them) */
    if (ctx->dnssd) {
        dnssd_register_raop(ctx->dnssd, port);
        dnssd_register_airplay(ctx->dnssd, port);
    }

    return (jint)port;
}

extern "C"
JNIEXPORT void JNICALL
Java_io_github_jqssun_airplay_bridge_NativeBridge_nativeStop(
        JNIEnv *env, jobject thiz, jlong handle) {

    server_ctx_t *ctx = (server_ctx_t *)(intptr_t)handle;
    if (!ctx || !ctx->raop) return;

    raop_stop_httpd(ctx->raop);
    LOGI("AirPlay server stopped");

    if (ctx->dnssd) {
        dnssd_unregister_raop(ctx->dnssd);
        dnssd_unregister_airplay(ctx->dnssd);
    }
}

extern "C"
JNIEXPORT void JNICALL
Java_io_github_jqssun_airplay_bridge_NativeBridge_nativeDestroy(
        JNIEnv *env, jobject thiz, jlong handle) {

    server_ctx_t *ctx = (server_ctx_t *)(intptr_t)handle;
    if (!ctx) return;

    /* release the /play response hold (if any) before joining the httpd thread via
       raop_destroy, so teardown can't block the caller for the hold timeout */
    ctx->cb_ctx.stopping = 1;
    pthread_mutex_lock(&ctx->cb_ctx.playback_info_lock);
    pthread_cond_broadcast(&ctx->cb_ctx.play_ready_cond);
    pthread_mutex_unlock(&ctx->cb_ctx.playback_info_lock);
    if (ctx->raop) {
        raop_destroy(ctx->raop);
        ctx->raop = NULL;
    }
    // engine teardown safe: raop_destroy() already ran, no decode() calls in flight
    AudioEngine *engine = ctx->cb_ctx.audio_engine;
    ctx->cb_ctx.audio_engine = NULL;
    if (engine) audio_engine_destroy(engine);
    if (ctx->dnssd) {
        dnssd_destroy(ctx->dnssd);
        ctx->dnssd = NULL;
    }
    android_callbacks_destroy(&ctx->cb_ctx, env);
    delete ctx;
}

extern "C"
JNIEXPORT void JNICALL
Java_io_github_jqssun_airplay_bridge_NativeBridge_nativeSetDisplaySize(
        JNIEnv *env, jobject thiz, jlong handle, jint w, jint h, jint fps) {

    server_ctx_t *ctx = (server_ctx_t *)(intptr_t)handle;
    if (!ctx || !ctx->raop) return;

    raop_set_plist(ctx->raop, "width", w);
    raop_set_plist(ctx->raop, "height", h);
    raop_set_plist(ctx->raop, "refreshRate", fps);
}

/* Returns a HashMap<String, String> of TXT records */
static jobject _build_txt_map(JNIEnv *env, dnssd_t *dnssd, int is_raop) {
    jclass mapClass = env->FindClass("java/util/HashMap");
    if (!mapClass) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return NULL;
    }
    jmethodID mapInit = env->GetMethodID(mapClass, "<init>", "()V");
    jmethodID mapPut = env->GetMethodID(mapClass, "put",
        "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    if (!mapInit || !mapPut) {
        env->DeleteLocalRef(mapClass);
        return NULL;
    }
    jobject map = env->NewObject(mapClass, mapInit);
    if (!map) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        env->DeleteLocalRef(mapClass);
        return NULL;
    }

    int count = is_raop ? android_dnssd_get_raop_txt_count(dnssd)
                        : android_dnssd_get_airplay_txt_count(dnssd);

    for (int i = 0; i < count; i++) {
        const char *key = is_raop ? android_dnssd_get_raop_txt_key(dnssd, i)
                                  : android_dnssd_get_airplay_txt_key(dnssd, i);
        const char *val = is_raop ? android_dnssd_get_raop_txt_val(dnssd, i)
                                  : android_dnssd_get_airplay_txt_val(dnssd, i);
        if (key && val) {
            jstring jkey = env->NewStringUTF(key);
            jstring jval = env->NewStringUTF(val);
            env->CallObjectMethod(map, mapPut, jkey, jval);
            env->DeleteLocalRef(jkey);
            env->DeleteLocalRef(jval);
        }
    }

    env->DeleteLocalRef(mapClass);
    return map;
}

extern "C"
JNIEXPORT jobject JNICALL
Java_io_github_jqssun_airplay_bridge_NativeBridge_nativeGetRaopTxtRecords(
        JNIEnv *env, jobject thiz, jlong handle) {

    server_ctx_t *ctx = (server_ctx_t *)(intptr_t)handle;
    if (!ctx || !ctx->dnssd) return NULL;
    return _build_txt_map(env, ctx->dnssd, 1);
}

extern "C"
JNIEXPORT jobject JNICALL
Java_io_github_jqssun_airplay_bridge_NativeBridge_nativeGetAirplayTxtRecords(
        JNIEnv *env, jobject thiz, jlong handle) {

    server_ctx_t *ctx = (server_ctx_t *)(intptr_t)handle;
    if (!ctx || !ctx->dnssd) return NULL;
    return _build_txt_map(env, ctx->dnssd, 0);
}

extern "C"
JNIEXPORT jstring JNICALL
Java_io_github_jqssun_airplay_bridge_NativeBridge_nativeGetRaopServiceName(
        JNIEnv *env, jobject thiz, jlong handle) {

    server_ctx_t *ctx = (server_ctx_t *)(intptr_t)handle;
    if (!ctx || !ctx->dnssd) return NULL;
    const char *name = android_dnssd_get_raop_servname(ctx->dnssd);
    return name ? env->NewStringUTF(name) : NULL;
}

extern "C"
JNIEXPORT jstring JNICALL
Java_io_github_jqssun_airplay_bridge_NativeBridge_nativeGetServerName(
        JNIEnv *env, jobject thiz, jlong handle) {

    server_ctx_t *ctx = (server_ctx_t *)(intptr_t)handle;
    if (!ctx || !ctx->dnssd) return NULL;
    int len = 0;
    const char *name = dnssd_get_name(ctx->dnssd, &len);
    return name ? env->NewStringUTF(name) : NULL;
}

extern "C"
JNIEXPORT void JNICALL
Java_io_github_jqssun_airplay_bridge_NativeBridge_nativeSetPlist(
        JNIEnv *env, jobject thiz, jlong handle, jstring key, jint value) {

    server_ctx_t *ctx = (server_ctx_t *)(intptr_t)handle;
    if (!ctx || !ctx->raop) return;
    const char *key_c = env->GetStringUTFChars(key, NULL);
    raop_set_plist(ctx->raop, key_c, value);
    env->ReleaseStringUTFChars(key, key_c);
}

extern "C"
JNIEXPORT void JNICALL
Java_io_github_jqssun_airplay_bridge_NativeBridge_nativeSetH265Enabled(
        JNIEnv *env, jobject thiz, jlong handle, jboolean enabled) {

    server_ctx_t *ctx = (server_ctx_t *)(intptr_t)handle;
    if (!ctx) return;
    ctx->cb_ctx.h265_enabled = enabled ? 1 : 0;
    /* Set DNS-SD feature bit 42 (SupportsScreenMultiCodec) for H265 */
    if (ctx->dnssd) {
        dnssd_set_airplay_features(ctx->dnssd, 42, enabled ? 1 : 0);
    }
}

/* hls plist gates raop.c's hls support; feature bits 0/4 advertise it over dns-sd */
extern "C"
JNIEXPORT void JNICALL
Java_io_github_jqssun_airplay_bridge_NativeBridge_nativeSetHlsEnabled(
        JNIEnv *env, jobject thiz, jlong handle, jboolean enabled) {

    server_ctx_t *ctx = (server_ctx_t *)(intptr_t)handle;
    if (!ctx || !ctx->raop) return;
    raop_set_plist(ctx->raop, "hls", enabled ? 1 : 0);
    if (ctx->dnssd) {
        dnssd_set_airplay_features(ctx->dnssd, 0, enabled ? 1 : 0);
        dnssd_set_airplay_features(ctx->dnssd, 4, enabled ? 1 : 0);
    }
}

extern "C"
JNIEXPORT void JNICALL
Java_io_github_jqssun_airplay_bridge_NativeBridge_nativeSetLang(
        JNIEnv *env, jobject thiz, jlong handle, jstring requested, jstring subtitles, jstring system) {

    server_ctx_t *ctx = (server_ctx_t *)(intptr_t)handle;
    if (!ctx || !ctx->raop) return;
    auto take = [env](jstring js, std::string &out) {
        const char *c = env->GetStringUTFChars(js, NULL);
        out = c;
        env->ReleaseStringUTFChars(js, c);
    };
    take(requested, ctx->lang_requested);
    take(subtitles, ctx->lang_subtitles);
    take(system, ctx->lang_system);
    raop_set_lang(ctx->raop, ctx->lang_requested.c_str(), ctx->lang_subtitles.c_str(), ctx->lang_system.c_str());
}

extern "C"
JNIEXPORT void JNICALL
Java_io_github_jqssun_airplay_bridge_NativeBridge_nativeSetFeature(
        JNIEnv *env, jobject thiz, jlong handle, jint bit, jboolean enabled) {

    server_ctx_t *ctx = (server_ctx_t *)(intptr_t)handle;
    if (!ctx || !ctx->dnssd) return;
    dnssd_set_airplay_features(ctx->dnssd, bit, enabled ? 1 : 0);
}

extern "C"
JNIEXPORT void JNICALL
Java_io_github_jqssun_airplay_bridge_NativeBridge_nativeSetCodecs(
        JNIEnv *env, jobject thiz, jlong handle, jboolean alac, jboolean aac) {

    server_ctx_t *ctx = (server_ctx_t *)(intptr_t)handle;
    if (!ctx || !ctx->dnssd) return;
    android_dnssd_set_codecs(ctx->dnssd, alac ? 1 : 0, aac ? 1 : 0);
}

extern "C"
JNIEXPORT void JNICALL
Java_io_github_jqssun_airplay_bridge_NativeBridge_nativeUpdatePlaybackInfo(
        JNIEnv *env, jobject thiz, jlong handle,
        jfloat position, jfloat duration, jfloat rate, jboolean readyToPlay) {

    server_ctx_t *ctx = (server_ctx_t *)(intptr_t)handle;
    if (!ctx) return;
    android_callbacks_update_playback_info(&ctx->cb_ctx, position, duration, rate, readyToPlay ? 1 : 0);
}

extern "C"
JNIEXPORT void JNICALL
Java_io_github_jqssun_airplay_bridge_NativeBridge_nativeSetDefaultStreamValues(
        JNIEnv *env, jobject thiz, jint sampleRate, jint framesPerBurst) {
    audio_engine_set_default_stream_values(sampleRate, framesPerBurst);
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_io_github_jqssun_airplay_bridge_NativeBridge_nativeServerAudioStart(
        JNIEnv *env, jobject thiz, jlong handle) {
    server_ctx_t *ctx = (server_ctx_t *)(intptr_t)handle;
    if (!ctx || !ctx->cb_ctx.audio_engine) return JNI_FALSE;
    return audio_engine_start(ctx->cb_ctx.audio_engine) ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT void JNICALL
Java_io_github_jqssun_airplay_bridge_NativeBridge_nativeServerAudioStop(
        JNIEnv *env, jobject thiz, jlong handle) {
    server_ctx_t *ctx = (server_ctx_t *)(intptr_t)handle;
    if (ctx && ctx->cb_ctx.audio_engine) audio_engine_pause(ctx->cb_ctx.audio_engine);
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_io_github_jqssun_airplay_bridge_NativeBridge_nativeServerAudioConfigure(
        JNIEnv *env, jobject thiz, jlong handle, jint cushionMs, jint percentilePct,
        jint oboeBufferFrames, jboolean forceSwAlac, jboolean forceSwAac, jboolean realtimePriority,
        jboolean lowLatency, jboolean benchmarkLog) {
    server_ctx_t *ctx = (server_ctx_t *)(intptr_t)handle;
    if (!ctx || !ctx->cb_ctx.audio_engine) return JNI_FALSE;
    return audio_engine_configure(ctx->cb_ctx.audio_engine, cushionMs, percentilePct,
                                  oboeBufferFrames, forceSwAlac, forceSwAac, realtimePriority,
                                  lowLatency, benchmarkLog) ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT void JNICALL
Java_io_github_jqssun_airplay_bridge_NativeBridge_nativeServerAudioFormat(
        JNIEnv *env, jobject thiz, jlong handle, jint ct, jint spf) {
    server_ctx_t *ctx = (server_ctx_t *)(intptr_t)handle;
    if (ctx && ctx->cb_ctx.audio_engine) audio_engine_on_format(ctx->cb_ctx.audio_engine, ct, spf);
}

extern "C"
JNIEXPORT void JNICALL
Java_io_github_jqssun_airplay_bridge_NativeBridge_nativeServerAudioFlush(
        JNIEnv *env, jobject thiz, jlong handle) {
    // content switch signalled by the video stream (its pts jumped): drop the
    // jitter buffer's old timeline so the new content doesn't have to play out
    // the previous one first
    server_ctx_t *ctx = (server_ctx_t *)(intptr_t)handle;
    if (ctx && ctx->cb_ctx.audio_engine) audio_engine_flush(ctx->cb_ctx.audio_engine);
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_io_github_jqssun_airplay_bridge_NativeBridge_nativeServerAudioDebug(
        JNIEnv *env, jobject thiz, jlong handle, jobject buf) {
    server_ctx_t *ctx = (server_ctx_t *)(intptr_t)handle;
    if (!ctx || !ctx->cb_ctx.audio_engine || !buf) return JNI_FALSE;
    void *addr = env->GetDirectBufferAddress(buf);
    jlong cap = env->GetDirectBufferCapacity(buf);
    if (!addr || cap <= 0) return JNI_FALSE;
    return audio_engine_get_debug(ctx->cb_ctx.audio_engine, addr, (size_t)cap) ? JNI_TRUE : JNI_FALSE;
}

/* A/V sync: fill out[0..3] = {1, ptsNs, wallNs, epoch} when an anchor exists.
 * false = no anchor yet; the video renderer then keeps its current one */
extern "C"
JNIEXPORT jboolean JNICALL
Java_io_github_jqssun_airplay_bridge_NativeBridge_nativeServerAudioAnchor(
        JNIEnv *env, jobject thiz, jlong handle, jlongArray out) {
    server_ctx_t *ctx = (server_ctx_t *)(intptr_t)handle;
    if (!ctx || !ctx->cb_ctx.audio_engine || !out) return JNI_FALSE;
    if (env->GetArrayLength(out) < 4) return JNI_FALSE;
    int64_t ptsNs = 0, wallNs = 0;
    uint32_t epoch = 0;
    if (!audio_engine_playout_anchor(ctx->cb_ctx.audio_engine, &ptsNs, &wallNs, &epoch)) {
        return JNI_FALSE;
    }
    jlong buf[4] = {1, (jlong)ptsNs, (jlong)wallNs, (jlong)epoch};
    env->SetLongArrayRegion(out, 0, 4, buf);
    return JNI_TRUE;
}

/* multi-session: how many concurrent AirPlay senders are accepted */
extern "C"
JNIEXPORT void JNICALL
Java_io_github_jqssun_airplay_bridge_NativeBridge_nativeSetMaxSessions(
        JNIEnv *env, jobject thiz, jlong handle, jint maxSessions) {
    server_ctx_t *ctx = (server_ctx_t *)(intptr_t)handle;
    if (!ctx || !ctx->raop) return;
    raop_set_max_sessions(ctx->raop, (int)maxSessions);
    LOGI("max sessions = %d", (int)maxSessions);
}

/* which sender is allowed to reach the audio output; 0 = first come first served */
extern "C"
JNIEXPORT void JNICALL
Java_io_github_jqssun_airplay_bridge_NativeBridge_nativeSetAudioOwner(
        JNIEnv *env, jobject thiz, jlong handle, jlong sessionId) {
    server_ctx_t *ctx = (server_ctx_t *)(intptr_t)handle;
    if (!ctx) return;
    ctx->cb_ctx.audio_owner_sid = (int64_t)sessionId;
    LOGI("audio owner = %lld", (long long)sessionId);
}
