#ifndef ANDROID_RAOP_CALLBACKS_H
#define ANDROID_RAOP_CALLBACKS_H

#include <jni.h>
#include <pthread.h>
#include "raop.h"
#include "audio_engine.h"

#define VIDEO_FRAME_SLOTS 2

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    JavaVM *jvm;
    jobject callback_obj;
    raop_t *raop;
    jmethodID on_video_data;
    jmethodID on_audio_format;
    jmethodID on_video_size;
    jmethodID on_volume_change;
    jmethodID on_client_volume;
    jmethodID on_audio_teardown;
    jmethodID on_conn_init;
    jmethodID on_conn_destroy;
    jmethodID on_conn_reset;
    jmethodID on_display_pin;
    jmethodID on_metadata;
    jmethodID on_coverart;
    jmethodID on_progress;
    jmethodID on_dacp_id;
    jmethodID on_mirror_running;
    jmethodID on_client_info;
    jmethodID on_video_play;
    jmethodID on_video_scrub;
    jmethodID on_video_rate;
    jmethodID on_video_stop;
    jmethodID on_video_session_poll;
    int h265_enabled;
    int require_pin;
    /* pairing-store access can race across concurrent pair-setup connections */
    pthread_mutex_t register_lock;
    char *registered_keys[16];
    int registered_count;
    /* playback info snapshot pushed by kotlin, read on the native httpd thread */
    pthread_mutex_t playback_info_lock;
    double playback_position;
    double playback_duration;
    float playback_rate;
    int playback_ready;
    /* holds the /play response until the player is ready, so self-driven senders (macOS)
       establish their timeline after the real duration is known, not at duration 0 */
    pthread_cond_t play_ready_cond;
    int play_ready;
    AudioEngine *audio_engine;
    /* set before nativeDestroy tears the ctx down: releases the /play response hold
       immediately so httpd_stop's thread join cannot block the main thread for the
       full 10s hold timeout (ANR on service destroy) */
    volatile int stopping;
    /* zero-copy video frame delivery: one reusable direct ByteBuffer per session
       slot (two mirror sessions run on two native threads, so a single shared
       buffer would hand session A's bytes to session B), grown on demand and
       protected by video_frame_lock. The Kotlin consumer copies the frame into the
       MediaCodec input buffer synchronously inside onVideoData, so the buffer is
       free for reuse as soon as that JNI call returns. */
    jobject video_frame_buf[VIDEO_FRAME_SLOTS]; /* global refs to DirectByteBuffers */
    void *video_frame_addr[VIDEO_FRAME_SLOTS];
    jsize video_frame_cap[VIDEO_FRAME_SLOTS];
    uint64_t video_frame_sid[VIDEO_FRAME_SLOTS]; /* 0 = slot unused */
    pthread_mutex_t video_frame_lock;
    /* audio arbitration: only this session's packets reach the AudioEngine. 0 =
       nobody chosen yet (first session to arrive becomes the owner). */
    volatile int64_t audio_owner_sid;
} android_callback_ctx_t;

void android_callbacks_init(android_callback_ctx_t *ctx, JNIEnv *env, jobject callback_obj);
void android_callbacks_destroy(android_callback_ctx_t *ctx, JNIEnv *env);
void android_callbacks_fill(raop_callbacks_t *cbs, android_callback_ctx_t *ctx);
void android_callbacks_update_playback_info(android_callback_ctx_t *ctx, double position,
                                             double duration, float rate, int ready);

#ifdef __cplusplus
}
#endif

#endif
