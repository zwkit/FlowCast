/*
 * Implements raop_callbacks_t by forwarding to Java/Kotlin via JNI.
 * All callbacks fire from RAOP's internal pthreads, so we AttachCurrentThread.
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <android/log.h>
#include "android_raop_callbacks.h"
#include "audio_engine.h"

#define TAG "AirPlayNative"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static JNIEnv *_get_env(android_callback_ctx_t *ctx) {
    JNIEnv *env = NULL;
    int status = (*ctx->jvm)->GetEnv(ctx->jvm, (void **)&env, JNI_VERSION_1_6);
    if (status == JNI_EDETACHED) {
        (*ctx->jvm)->AttachCurrentThread(ctx->jvm, &env, NULL);
    }
    /* Clear any pending exception from a previous callback on this thread,
       otherwise JNI calls like NewByteArray will fatally abort. */
    if (env && (*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
    }
    return env;
}

void android_callbacks_init(android_callback_ctx_t *ctx, JNIEnv *env, jobject callback_obj) {
    (*env)->GetJavaVM(env, &ctx->jvm);
    ctx->callback_obj = (*env)->NewGlobalRef(env, callback_obj);
    ctx->h265_enabled = 1;
    ctx->require_pin = 0;
    ctx->registered_count = 0;
    ctx->stopping = 0;
    ctx->audio_engine = NULL;
    ctx->audio_owner_sid = 0;
    for (int slot = 0; slot < VIDEO_FRAME_SLOTS; slot++) {
        ctx->video_frame_buf[slot] = NULL;
        ctx->video_frame_addr[slot] = NULL;
        ctx->video_frame_cap[slot] = 0;
        ctx->video_frame_sid[slot] = 0;
    }
    memset(ctx->registered_keys, 0, sizeof(ctx->registered_keys));

    pthread_mutex_init(&ctx->playback_info_lock, NULL);
    pthread_cond_init(&ctx->play_ready_cond, NULL);
    pthread_mutex_init(&ctx->register_lock, NULL);
    pthread_mutex_init(&ctx->video_frame_lock, NULL);
    ctx->play_ready = 0;
    ctx->playback_position = 0.0;
    /* -1.0 is the video finished sentinel, reserved for _video_stop */
    ctx->playback_duration = 0.0;
    ctx->playback_rate = 0.0f;
    ctx->playback_ready = 0;

    jclass cls = (*env)->GetObjectClass(env, callback_obj);
    ctx->on_video_data = (*env)->GetMethodID(env, cls, "onVideoData", "(JLjava/nio/ByteBuffer;IJZ)V");
    ctx->on_audio_format = (*env)->GetMethodID(env, cls, "onAudioFormat", "(JIIZ)V");
    ctx->on_video_size = (*env)->GetMethodID(env, cls, "onVideoSize", "(JFFFF)V");
    ctx->on_volume_change = (*env)->GetMethodID(env, cls, "onVolumeChange", "(F)V");
    ctx->on_client_volume = (*env)->GetMethodID(env, cls, "onClientVolume", "()F");
    ctx->on_audio_teardown = (*env)->GetMethodID(env, cls, "onAudioTeardown", "()V");
    ctx->on_conn_init = (*env)->GetMethodID(env, cls, "onConnectionInit", "(J)V");
    ctx->on_conn_destroy = (*env)->GetMethodID(env, cls, "onConnectionDestroy", "(J)V");
    ctx->on_conn_reset = (*env)->GetMethodID(env, cls, "onConnectionReset", "(JI)V");
    ctx->on_display_pin = (*env)->GetMethodID(env, cls, "onDisplayPin", "(Ljava/lang/String;)V");
    ctx->on_metadata = (*env)->GetMethodID(env, cls, "onMetadata", "([B)V");
    ctx->on_coverart = (*env)->GetMethodID(env, cls, "onCoverArt", "([B)V");
    ctx->on_progress = (*env)->GetMethodID(env, cls, "onProgress", "(JJJ)V");
    ctx->on_dacp_id = (*env)->GetMethodID(env, cls, "onDacpId", "(Ljava/lang/String;Ljava/lang/String;)V");
    ctx->on_mirror_running = (*env)->GetMethodID(env, cls, "onMirrorRunning", "(JZ)V");
    ctx->on_client_info = (*env)->GetMethodID(env, cls, "onClientInfo", "(JLjava/lang/String;Ljava/lang/String;)V");
    ctx->on_video_play = (*env)->GetMethodID(env, cls, "onVideoPlay", "(Ljava/lang/String;F)V");
    ctx->on_video_scrub = (*env)->GetMethodID(env, cls, "onVideoScrub", "(F)V");
    ctx->on_video_rate = (*env)->GetMethodID(env, cls, "onVideoRate", "(F)V");
    ctx->on_video_stop = (*env)->GetMethodID(env, cls, "onVideoStop", "()V");
    ctx->on_video_session_poll = (*env)->GetMethodID(env, cls, "onVideoSessionPoll", "()V");
    (*env)->DeleteLocalRef(env, cls);
}

void android_callbacks_destroy(android_callback_ctx_t *ctx, JNIEnv *env) {
    if (ctx->callback_obj) {
        (*env)->DeleteGlobalRef(env, ctx->callback_obj);
        ctx->callback_obj = NULL;
    }
    for (int slot = 0; slot < VIDEO_FRAME_SLOTS; slot++) {
        if (ctx->video_frame_buf[slot]) {
            (*env)->DeleteGlobalRef(env, ctx->video_frame_buf[slot]);
            ctx->video_frame_buf[slot] = NULL;
        }
        if (ctx->video_frame_addr[slot]) {
            free(ctx->video_frame_addr[slot]);
            ctx->video_frame_addr[slot] = NULL;
        }
        ctx->video_frame_cap[slot] = 0;
        ctx->video_frame_sid[slot] = 0;
    }
    pthread_mutex_destroy(&ctx->video_frame_lock);
    pthread_mutex_lock(&ctx->register_lock);
    for (int i = 0; i < ctx->registered_count; i++) {
        free(ctx->registered_keys[i]);
        ctx->registered_keys[i] = NULL;
    }
    ctx->registered_count = 0;
    pthread_mutex_unlock(&ctx->register_lock);
    pthread_mutex_destroy(&ctx->register_lock);
    pthread_cond_destroy(&ctx->play_ready_cond);
    pthread_mutex_destroy(&ctx->playback_info_lock);
}

void android_callbacks_update_playback_info(android_callback_ctx_t *ctx, double position,
                                             double duration, float rate, int ready) {
    pthread_mutex_lock(&ctx->playback_info_lock);
    ctx->playback_position = position;
    ctx->playback_duration = duration;
    ctx->playback_rate = rate;
    ctx->playback_ready = ready;
    if (ready && !ctx->play_ready) {
        ctx->play_ready = 1;
        pthread_cond_signal(&ctx->play_ready_cond);
    }
    pthread_mutex_unlock(&ctx->playback_info_lock);
}

/* --- RAOP callback implementations --- */

static void _audio_process(void *cls, uint64_t session_id, raop_ntp_t *ntp, audio_decode_struct *data) {
    android_callback_ctx_t *ctx = (android_callback_ctx_t *)cls;
    if (!ctx->audio_engine || !data->data || data->data_len <= 0) return;
    /* only one sender may be heard at a time: everything that is not the current
       audio owner is dropped here. Dropping is safe - raop_rtp already handled
       retransmission/ACK for this packet, it just never reaches the playout buffer. */
    int64_t owner = ctx->audio_owner_sid;
    if (owner > 0 && (int64_t) session_id != owner) return;
    audio_engine_decode(ctx->audio_engine, data->data, data->data_len,
                        (int)data->ct, (int64_t)data->ntp_time_local);
}

/* Grows the shared direct frame buffer to at least need bytes. Caller holds
   ctx->video_frame_lock. Returns 0 on success, -1 on allocation failure. */
static int _ensure_video_frame_buf(android_callback_ctx_t *ctx, JNIEnv *env, int slot, jsize need) {
    if (ctx->video_frame_buf[slot] && ctx->video_frame_cap[slot] >= need) return 0;
    jsize cap = ctx->video_frame_cap[slot] > 0 ? ctx->video_frame_cap[slot] : (jsize)(256 * 1024);
    while (cap < need) cap *= 2;
    void *addr = malloc((size_t)cap);
    if (!addr) return -1;
    jobject local = (*env)->NewDirectByteBuffer(env, addr, cap);
    if (!local) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        free(addr);
        return -1;
    }
    jobject global = (*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);
    if (!global) {
        free(addr);
        return -1;
    }
    if (ctx->video_frame_buf[slot]) (*env)->DeleteGlobalRef(env, ctx->video_frame_buf[slot]);
    if (ctx->video_frame_addr[slot]) free(ctx->video_frame_addr[slot]);
    ctx->video_frame_buf[slot] = global;
    ctx->video_frame_addr[slot] = addr;
    ctx->video_frame_cap[slot] = cap;
    return 0;
}

/* Maps a session to one of the fixed frame-buffer slots. Caller holds
   video_frame_lock. A session keeps its slot until it disappears. */
static int _video_slot_for(android_callback_ctx_t *ctx, uint64_t session_id) {
    for (int slot = 0; slot < VIDEO_FRAME_SLOTS; slot++) {
        if (ctx->video_frame_sid[slot] == session_id) return slot;
    }
    for (int slot = 0; slot < VIDEO_FRAME_SLOTS; slot++) {
        if (ctx->video_frame_sid[slot] == 0) {
            ctx->video_frame_sid[slot] = session_id;
            return slot;
        }
    }
    /* more sessions than slots: reuse slot 0 (serialised by the lock) */
    return 0;
}

static void _video_process(void *cls, uint64_t session_id, raop_ntp_t *ntp, video_decode_struct *data) {
    android_callback_ctx_t *ctx = (android_callback_ctx_t *)cls;
    JNIEnv *env = _get_env(ctx);
    if (!env || !data->data || data->data_len <= 0) return;
    /* raop_rtp_mirror marks undecryptable packets by setting byte 0 (start code) to 1 */
    if (data->data[0]) {
        LOGE("video packet decryption failed, dropping %d bytes", data->data_len);
        return;
    }

    /* Zero-copy frame delivery: fill one reusable direct ByteBuffer instead of
       allocating a jbyteArray per frame (at 1080p60 that was ~6MB/s of allocation
       churn plus 60 JNI array creations per second). The lock is held across the
       JNI call and mirrors VideoRenderer.feedFrame's global serialization on the
       Kotlin side, so it costs no throughput; the consumer copies the frame into
       the MediaCodec input buffer synchronously and the buffer is then reusable. */
    pthread_mutex_lock(&ctx->video_frame_lock);
    int slot = _video_slot_for(ctx, session_id);
    if (_ensure_video_frame_buf(ctx, env, slot, (jsize)data->data_len) != 0) {
        pthread_mutex_unlock(&ctx->video_frame_lock);
        LOGE("video frame buffer alloc failed (%d bytes), dropping frame", data->data_len);
        return;
    }
    memcpy(ctx->video_frame_addr[slot], data->data, (size_t)data->data_len);
    (*env)->CallVoidMethod(env, ctx->callback_obj, ctx->on_video_data,
                           (jlong)session_id, ctx->video_frame_buf[slot], (jint)data->data_len,
                           (jlong)data->ntp_time_local, (jboolean)data->is_h265);
    pthread_mutex_unlock(&ctx->video_frame_lock);
}

static void _conn_init(void *cls, uint64_t session_id) {
    android_callback_ctx_t *ctx = (android_callback_ctx_t *)cls;
    /* fires for every connection including the player's own hls fetches: clear only a stale sentinel */
    pthread_mutex_lock(&ctx->playback_info_lock);
    if (ctx->playback_duration == -1.0) {
        ctx->playback_position = 0.0;
        ctx->playback_duration = 0.0;
        ctx->playback_rate = 0.0f;
        ctx->playback_ready = 0;
    }
    pthread_mutex_unlock(&ctx->playback_info_lock);
    JNIEnv *env = _get_env(ctx);
    if (!env) return;
    (*env)->CallVoidMethod(env, ctx->callback_obj, ctx->on_conn_init, (jlong)session_id);
}

static void _conn_destroy(void *cls, uint64_t session_id) {
    android_callback_ctx_t *ctx = (android_callback_ctx_t *)cls;
    /* release the frame buffer slot so a later session can claim it */
    pthread_mutex_lock(&ctx->video_frame_lock);
    for (int slot = 0; slot < VIDEO_FRAME_SLOTS; slot++) {
        if (ctx->video_frame_sid[slot] == session_id) ctx->video_frame_sid[slot] = 0;
    }
    pthread_mutex_unlock(&ctx->video_frame_lock);
    JNIEnv *env = _get_env(ctx);
    if (!env) return;
    (*env)->CallVoidMethod(env, ctx->callback_obj, ctx->on_conn_destroy, (jlong)session_id);
}

static void _conn_reset(void *cls, uint64_t session_id, int reason) {
    android_callback_ctx_t *ctx = (android_callback_ctx_t *)cls;
    JNIEnv *env = _get_env(ctx);
    if (!env) return;
    (*env)->CallVoidMethod(env, ctx->callback_obj, ctx->on_conn_reset, (jlong)session_id, (jint)reason);
}

static void _audio_set_volume(void *cls, float volume) {
    android_callback_ctx_t *ctx = (android_callback_ctx_t *)cls;
    JNIEnv *env = _get_env(ctx);
    if (!env) return;
    (*env)->CallVoidMethod(env, ctx->callback_obj, ctx->on_volume_change, (jfloat)volume);
}

static void _audio_get_format(void *cls, uint64_t session_id, unsigned char *ct, unsigned short *spf,
                               bool *usingScreen, bool *isMedia, uint64_t *audioFormat) {
    android_callback_ctx_t *ctx = (android_callback_ctx_t *)cls;
    JNIEnv *env = _get_env(ctx);
    if (!env) return;
    (*env)->CallVoidMethod(env, ctx->callback_obj, ctx->on_audio_format,
                           (jlong)session_id, (jint)*ct, (jint)*spf, (jboolean)*usingScreen);
}

static void _video_report_size(void *cls, uint64_t session_id, float *w_src, float *h_src, float *w, float *h) {
    android_callback_ctx_t *ctx = (android_callback_ctx_t *)cls;
    JNIEnv *env = _get_env(ctx);
    if (!env) return;
    (*env)->CallVoidMethod(env, ctx->callback_obj, ctx->on_video_size,
                           (jlong)session_id, (jfloat)*w_src, (jfloat)*h_src, (jfloat)*w, (jfloat)*h);
}

static void _display_pin(void *cls, char *pin) {
    android_callback_ctx_t *ctx = (android_callback_ctx_t *)cls;
    
    /* certain senders trigger pair-pin-start even when we advertise no auth */
    if (!ctx->require_pin) return;

    JNIEnv *env = _get_env(ctx);
    if (!env) return;
    jstring jpin = (*env)->NewStringUTF(env, pin);
    (*env)->CallVoidMethod(env, ctx->callback_obj, ctx->on_display_pin, jpin);
    (*env)->DeleteLocalRef(env, jpin);
}

/* Stubs for less critical callbacks */
static void _noop(void *cls) { (void)cls; }
static void _video_pause(void *cls) { LOGI("video_pause"); }
static void _video_resume(void *cls) { LOGI("video_resume"); }
static void _conn_feedback(void *cls) { (void)cls; }
static void _video_stop(void *cls);
static void _video_reset(void *cls, reset_type_t t) {
    android_callback_ctx_t *ctx = (android_callback_ctx_t *)cls;
    LOGI("video_reset %d", t);
    if (t == RESET_TYPE_HLS_SHUTDOWN || t == RESET_TYPE_HLS_EOS) {
        _video_stop(cls);
    }
    if (t == RESET_TYPE_HLS_CONN_CLOSED) {
        /* abandoned only if paused and ready: rate alone is also 0 while buffering */
        pthread_mutex_lock(&ctx->playback_info_lock);
        int paused = ctx->playback_ready && ctx->playback_rate <= 0.0f;
        pthread_mutex_unlock(&ctx->playback_info_lock);
        if (paused) {
            _video_stop(cls);
        }
    }
    if (t == RESET_TYPE_HLS_SHUTDOWN && ctx->raop) {
        raop_remove_hls_connections(ctx->raop);
    }
}
static void _audio_flush(void *cls, uint64_t session_id) {
    android_callback_ctx_t *ctx = (android_callback_ctx_t *)cls;
    /* a muted session's FLUSH must not cut the owner's audio mid-playback */
    int64_t owner = ctx->audio_owner_sid;
    if (owner > 0 && (int64_t) session_id != owner) return;
    LOGI("audio_flush (session %llu)", (unsigned long long) session_id);
    /* sender FLUSH (seek / new stream): drop the buffered audio so the new timeline
       doesn't have to play out the old one first */
    if (ctx->audio_engine) audio_engine_flush(ctx->audio_engine);
}

static void _audio_stop_coverart_rendering(void *cls) {
    android_callback_ctx_t *ctx = (android_callback_ctx_t *)cls;
    LOGI("audio_teardown");
    JNIEnv *env = _get_env(ctx);
    if (!env) return;
    (*env)->CallVoidMethod(env, ctx->callback_obj, ctx->on_audio_teardown);
}
static void _video_flush(void *cls, uint64_t session_id) { LOGI("video_flush (session %llu)", (unsigned long long) session_id); }
static double _audio_set_client_volume(void *cls) {
    android_callback_ctx_t *ctx = (android_callback_ctx_t *)cls;
    JNIEnv *env = _get_env(ctx);
    if (!env) return 0.0;
    return (double)(*env)->CallFloatMethod(env, ctx->callback_obj, ctx->on_client_volume);
}
static void _audio_set_metadata(void *cls, const void *buf, int len) {
    android_callback_ctx_t *ctx = (android_callback_ctx_t *)cls;
    JNIEnv *env = _get_env(ctx);
    if (!env || !buf || len <= 0) return;
    jbyteArray arr = (*env)->NewByteArray(env, len);
    if (!arr) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        return;
    }
    (*env)->SetByteArrayRegion(env, arr, 0, len, (jbyte *)buf);
    (*env)->CallVoidMethod(env, ctx->callback_obj, ctx->on_metadata, arr);
    (*env)->DeleteLocalRef(env, arr);
}

static void _audio_set_coverart(void *cls, const void *buf, int len) {
    android_callback_ctx_t *ctx = (android_callback_ctx_t *)cls;
    JNIEnv *env = _get_env(ctx);
    if (!env || !buf || len <= 0) return;
    jbyteArray arr = (*env)->NewByteArray(env, len);
    if (!arr) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        return;
    }
    (*env)->SetByteArrayRegion(env, arr, 0, len, (jbyte *)buf);
    (*env)->CallVoidMethod(env, ctx->callback_obj, ctx->on_coverart, arr);
    (*env)->DeleteLocalRef(env, arr);
}

static void _audio_remote_control_id(void *cls, const char *dacp_id, const char *active_remote) {
    android_callback_ctx_t *ctx = (android_callback_ctx_t *)cls;
    JNIEnv *env = _get_env(ctx);
    if (!env) return;
    jstring jdacp = (*env)->NewStringUTF(env, dacp_id ? dacp_id : "");
    jstring jremote = (*env)->NewStringUTF(env, active_remote ? active_remote : "");
    (*env)->CallVoidMethod(env, ctx->callback_obj, ctx->on_dacp_id, jdacp, jremote);
    (*env)->DeleteLocalRef(env, jdacp);
    (*env)->DeleteLocalRef(env, jremote);
}

static void _audio_set_progress(void *cls, uint32_t *start, uint32_t *curr, uint32_t *end) {
    android_callback_ctx_t *ctx = (android_callback_ctx_t *)cls;
    JNIEnv *env = _get_env(ctx);
    if (!env || !start || !curr || !end) return;
    /* start/curr/end are stream frames at the engine's sample rate; convert to
       milliseconds here so kotlin doesn't have to assume a rate (frames at 48k
       would otherwise be reported ~8.8% too long) */
    int rate = ctx->audio_engine ? audio_engine_sample_rate(ctx->audio_engine) : 0;
    if (rate <= 0) rate = 44100;
    jlong s = (jlong)((uint64_t)*start * 1000ULL / (uint64_t)rate);
    jlong c = (jlong)((uint64_t)*curr * 1000ULL / (uint64_t)rate);
    jlong e = (jlong)((uint64_t)*end * 1000ULL / (uint64_t)rate);
    (*env)->CallVoidMethod(env, ctx->callback_obj, ctx->on_progress, s, c, e);
}

static void _mirror_video_running(void *cls, uint64_t session_id, bool running) {
    android_callback_ctx_t *ctx = (android_callback_ctx_t *)cls;
    JNIEnv *env = _get_env(ctx);
    if (!env) return;
    LOGI("mirror running: %d (session %llu)", running, (unsigned long long) session_id);
    (*env)->CallVoidMethod(env, ctx->callback_obj, ctx->on_mirror_running,
                           (jlong)session_id, (jboolean)running);
}

static void _report_client_request(void *cls, uint64_t session_id, char *device_id,
                                   char *model, char *name, bool *admit) {
    android_callback_ctx_t *ctx = (android_callback_ctx_t *)cls;
    if (admit) *admit = true;
    JNIEnv *env = _get_env(ctx);
    if (!env) return;
    jstring jmodel = (*env)->NewStringUTF(env, model ? model : "");
    jstring jname = (*env)->NewStringUTF(env, name ? name : "");
    (*env)->CallVoidMethod(env, ctx->callback_obj, ctx->on_client_info,
                           (jlong)session_id, jmodel, jname);
    (*env)->DeleteLocalRef(env, jmodel);
    (*env)->DeleteLocalRef(env, jname);
}
static void _register_client(void *cls, const char *device_id, const char *pk_str, const char *name) {
    android_callback_ctx_t *ctx = (android_callback_ctx_t *)cls;
    (void)device_id; (void)name;
    if (!pk_str) return;
    pthread_mutex_lock(&ctx->register_lock);
    if (ctx->registered_count >= 16) { pthread_mutex_unlock(&ctx->register_lock); return; }
    for (int i = 0; i < ctx->registered_count; i++) {
        if (ctx->registered_keys[i] && strcmp(ctx->registered_keys[i], pk_str) == 0) {
            pthread_mutex_unlock(&ctx->register_lock);
            return;
        }
    }
    ctx->registered_keys[ctx->registered_count++] = strdup(pk_str);
    pthread_mutex_unlock(&ctx->register_lock);
    LOGI("registered client pk (slot %d)", ctx->registered_count);
}

static bool _check_register(void *cls, const char *pk_str) {
    android_callback_ctx_t *ctx = (android_callback_ctx_t *)cls;
    bool found = false;
    pthread_mutex_lock(&ctx->register_lock);
    for (int i = 0; i < ctx->registered_count; i++) {
        if (ctx->registered_keys[i] && strcmp(ctx->registered_keys[i], pk_str) == 0) { found = true; break; }
    }
    pthread_mutex_unlock(&ctx->register_lock);
    return found;
}

/* --- AirPlay Video (HLS) playback callbacks --- */

static void _video_play(void *cls, const char *location, const float start_position) {
    android_callback_ctx_t *ctx = (android_callback_ctx_t *)cls;
    LOGI("video_play: %s @ %.2fs", location ? location : "(null)", start_position);
    pthread_mutex_lock(&ctx->playback_info_lock);
    ctx->play_ready = 0;
    pthread_mutex_unlock(&ctx->playback_info_lock);
    android_callbacks_update_playback_info(ctx, start_position, 0.0, 0.0f, 0);
    JNIEnv *env = _get_env(ctx);
    if (!env || !location) return;
    jstring jloc = (*env)->NewStringUTF(env, location);
    (*env)->CallVoidMethod(env, ctx->callback_obj, ctx->on_video_play, jloc, (jfloat)start_position);
    (*env)->DeleteLocalRef(env, jloc);
    /* self-driven senders (macOS) latch their scrubber timeline at /play; hold the response
       until the player reports ready so that read must carry the real duration */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 10; // hold for max 10s
    pthread_mutex_lock(&ctx->playback_info_lock);
    /* ctx->stopping is set by nativeDestroy before raop_destroy joins the httpd thread:
       without it the join would block the main thread for the full hold timeout */
    while (!ctx->play_ready && !ctx->stopping) {
        if (pthread_cond_timedwait(&ctx->play_ready_cond, &ctx->playback_info_lock, &ts) == ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&ctx->playback_info_lock);
}

static void _video_scrub(void *cls, const float position) {
    android_callback_ctx_t *ctx = (android_callback_ctx_t *)cls;
    JNIEnv *env = _get_env(ctx);
    if (!env) return;
    (*env)->CallVoidMethod(env, ctx->callback_obj, ctx->on_video_scrub, (jfloat)position);
}

static void _video_rate(void *cls, const float rate) {
    android_callback_ctx_t *ctx = (android_callback_ctx_t *)cls;
    JNIEnv *env = _get_env(ctx);
    if (!env) return;
    (*env)->CallVoidMethod(env, ctx->callback_obj, ctx->on_video_rate, (jfloat)rate);
}

static void _video_stop(void *cls) {
    android_callback_ctx_t *ctx = (android_callback_ctx_t *)cls;
    android_callbacks_update_playback_info(ctx, 0.0, -1.0, 0.0f, 0);
    JNIEnv *env = _get_env(ctx);
    if (!env) return;
    (*env)->CallVoidMethod(env, ctx->callback_obj, ctx->on_video_stop);
}

/* httpd thread: reads the kotlin-pushed snapshot, never calls into the player */
static void _video_acquire_playback_info(void *cls, playback_info_t *info) {
    android_callback_ctx_t *ctx = (android_callback_ctx_t *)cls;
    pthread_mutex_lock(&ctx->playback_info_lock);
    info->position = ctx->playback_position;
    info->duration = ctx->playback_duration;
    info->rate = ctx->playback_rate;
    info->ready_to_play = ctx->playback_ready;
    info->playback_buffer_empty = false;
    info->playback_buffer_full = true;
    info->playback_likely_to_keep_up = true;
    info->seek_start = 0.0;
    info->seek_duration = ctx->playback_duration > 0.0 ? ctx->playback_duration : 0.0;
    pthread_mutex_unlock(&ctx->playback_info_lock);
    /* polls are the earliest video-channel signal, starting ~1s before /play */
    JNIEnv *env = _get_env(ctx);
    if (env) {
        (*env)->CallVoidMethod(env, ctx->callback_obj, ctx->on_video_session_poll);
    }
}

static float _video_playlist_remove(void *cls) {
    android_callback_ctx_t *ctx = (android_callback_ctx_t *)cls;
    pthread_mutex_lock(&ctx->playback_info_lock);
    double position = ctx->playback_position;
    pthread_mutex_unlock(&ctx->playback_info_lock);
    return (float) position;
}

static int _video_set_codec(void *cls, video_codec_t codec) {
    android_callback_ctx_t *ctx = (android_callback_ctx_t *)cls;
    LOGI("video_set_codec: %d (h265_enabled=%d)", codec, ctx->h265_enabled);
    if (codec == VIDEO_CODEC_H265 && !ctx->h265_enabled) return -1;
    return 0;
}

void android_callbacks_fill(raop_callbacks_t *cbs, android_callback_ctx_t *ctx) {
    memset(cbs, 0, sizeof(raop_callbacks_t));
    cbs->cls = ctx;

    cbs->audio_process = _audio_process;
    cbs->video_process = _video_process;
    cbs->video_pause = _video_pause;
    cbs->video_resume = _video_resume;
    cbs->conn_feedback = _conn_feedback;
    cbs->conn_reset = _conn_reset;
    cbs->video_reset = _video_reset;
    cbs->conn_init = _conn_init;
    cbs->conn_destroy = _conn_destroy;
    cbs->audio_flush = _audio_flush;
    cbs->video_flush = _video_flush;
    cbs->audio_set_client_volume = _audio_set_client_volume;
    cbs->audio_set_volume = _audio_set_volume;
    cbs->audio_set_metadata = _audio_set_metadata;
    cbs->audio_set_coverart = _audio_set_coverart;
    cbs->audio_stop_coverart_rendering = _audio_stop_coverart_rendering;
    cbs->audio_remote_control_id = _audio_remote_control_id;
    cbs->audio_set_progress = _audio_set_progress;
    cbs->audio_get_format = _audio_get_format;
    cbs->video_report_size = _video_report_size;
    cbs->mirror_video_running = _mirror_video_running;
    cbs->report_client_request = _report_client_request;
    cbs->display_pin = _display_pin;
    cbs->video_set_codec = _video_set_codec;
    cbs->on_video_play = _video_play;
    cbs->on_video_scrub = _video_scrub;
    cbs->on_video_rate = _video_rate;
    cbs->on_video_stop = _video_stop;
    cbs->on_video_acquire_playback_info = _video_acquire_playback_info;
    cbs->on_video_playlist_remove = _video_playlist_remove;
    if (ctx->require_pin) {
        cbs->check_register = _check_register;
        cbs->register_client = _register_client;
    }
}
