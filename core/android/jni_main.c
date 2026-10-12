/*
 * AirPlay TV - AirPlay screen mirroring receiver for Android TV
 * Copyright (C) 2026 besliky
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

/*
 * JNI bridge between the Kotlin service and the native receiver.
 *
 * The boundary is kept narrow: configuration and control calls go down, a few
 * low-rate events come up. Media never crosses it except for decoded PCM,
 * which the AudioTrack writer pulls in blocks of several milliseconds, and
 * the occasional cover or photo as an encoded image.
 * Strings that can contain arbitrary Unicode travel as UTF-8 byte arrays,
 * because JNI's own string functions use modified UTF-8.
 */

#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "airplay.h"
#include "audio_pipeline.h"
#include "log.h"
#include "platform.h"
#include "util.h"
#include "video_decoder.h"

#define BRIDGE_CLASS "io/github/besliky/airplaytv/core/NativeBridge"
#define LOG_TAG "AirPlayTV"

static JavaVM *g_vm;
static jclass g_bridge;
static pthread_key_t g_env_key;

static struct {
    jmethodID session_started;
    jmethodID session_ended;
    jmethodID pin;
    jmethodID client_paired;
    jmethodID video_started;
    jmethodID video_stopped;
    jmethodID video_size;
    jmethodID audio_started;
    jmethodID audio_stopped;
    jmethodID volume;
    jmethodID track_info;
    jmethodID artwork;
    jmethodID progress;
    jmethodID remote_control;
    jmethodID photo;
    jmethodID photo_stop;
} g_cb;

static pthread_mutex_t g_server_lock = PTHREAD_MUTEX_INITIALIZER;
static airplay_server_t *g_server;

/* ------------------------------------------------------------------------- */
/* environment helpers                                                        */

static void detach_thread(void *unused) {
    (void) unused;
    if (g_vm) {
        (*g_vm)->DetachCurrentThread(g_vm);
    }
}

static JNIEnv *get_env(void) {
    JNIEnv *env = NULL;
    if (!g_vm) {
        return NULL;
    }
    jint r = (*g_vm)->GetEnv(g_vm, (void **) &env, JNI_VERSION_1_6);
    if (r == JNI_OK) {
        return env;
    }
    if (r == JNI_EDETACHED && (*g_vm)->AttachCurrentThread(g_vm, &env, NULL) == JNI_OK) {
        pthread_setspecific(g_env_key, env); /* detached again when the thread exits */
        return env;
    }
    return NULL;
}

static jbyteArray utf8_bytes(JNIEnv *env, const char *s) {
    if (!s) {
        return NULL;
    }
    jsize len = (jsize) strlen(s);
    jbyteArray arr = (*env)->NewByteArray(env, len);
    if (arr && len) {
        (*env)->SetByteArrayRegion(env, arr, 0, len, (const jbyte *) s);
    }
    return arr;
}

static void delete_local(JNIEnv *env, jobject ref) {
    if (ref) {
        (*env)->DeleteLocalRef(env, ref);
    }
}

/* A copy of data as a Java byte array, or NULL if there is none or it cannot be allocated. */
static jbyteArray byte_array(JNIEnv *env, const uint8_t *data, size_t len) {
    if (!data || len == 0 || len > 0x7fffffff) {
        return NULL;
    }
    jbyteArray arr = (*env)->NewByteArray(env, (jsize) len);
    if (arr) {
        (*env)->SetByteArrayRegion(env, arr, 0, (jsize) len, (const jbyte *) data);
    } else {
        (*env)->ExceptionClear(env); /* OutOfMemoryError: drop this one picture */
    }
    return arr;
}

static void check_exception(JNIEnv *env) {
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
    }
}

static void call_void(jmethodID m) {
    JNIEnv *env = get_env();
    if (env && m) {
        (*env)->CallStaticVoidMethod(env, g_bridge, m);
        check_exception(env);
    }
}

static void android_log_sink(log_level_t level, log_category_t category, const char *message) {
    static const int kPriority[] = { ANDROID_LOG_ERROR, ANDROID_LOG_WARN, ANDROID_LOG_INFO,
                                     ANDROID_LOG_DEBUG, ANDROID_LOG_VERBOSE };
    int prio = level <= LOGL_TRACE ? kPriority[level] : ANDROID_LOG_VERBOSE;
    __android_log_print(prio, LOG_TAG, "[%s] %s", log_category_name(category), message);
}

/* ------------------------------------------------------------------------- */
/* callbacks into Kotlin                                                      */

void platform_on_video_started(void) {
    call_void(g_cb.video_started);
}

void platform_on_video_stopped(void) {
    call_void(g_cb.video_stopped);
}

void platform_on_video_size(int width, int height) {
    JNIEnv *env = get_env();
    if (env) {
        (*env)->CallStaticVoidMethod(env, g_bridge, g_cb.video_size, (jint) width, (jint) height);
        check_exception(env);
    }
}

void platform_on_audio_started(int sample_rate, int channels, bool low_latency) {
    JNIEnv *env = get_env();
    if (env) {
        (*env)->CallStaticVoidMethod(env, g_bridge, g_cb.audio_started, (jint) sample_rate, (jint) channels,
                                     (jboolean) low_latency);
        check_exception(env);
    }
}

void platform_on_audio_stopped(void) {
    call_void(g_cb.audio_stopped);
}

void platform_on_volume(float gain) {
    JNIEnv *env = get_env();
    if (env) {
        (*env)->CallStaticVoidMethod(env, g_bridge, g_cb.volume, (jfloat) gain);
        check_exception(env);
    }
}

static void ev_session_started(void *ctx, const char *name, const char *model) {
    (void) ctx;
    JNIEnv *env = get_env();
    if (!env) {
        return;
    }
    jbyteArray n = utf8_bytes(env, name);
    jbyteArray m = utf8_bytes(env, model);
    (*env)->CallStaticVoidMethod(env, g_bridge, g_cb.session_started, n, m);
    check_exception(env);
    (*env)->DeleteLocalRef(env, n);
    (*env)->DeleteLocalRef(env, m);
}

static void ev_session_ended(void *ctx) {
    (void) ctx;
    call_void(g_cb.session_ended);
}

static void ev_pin(void *ctx, const char *pin) {
    (void) ctx;
    JNIEnv *env = get_env();
    if (!env) {
        return;
    }
    jbyteArray p = utf8_bytes(env, pin);
    (*env)->CallStaticVoidMethod(env, g_bridge, g_cb.pin, p);
    check_exception(env);
    if (p) {
        (*env)->DeleteLocalRef(env, p);
    }
}

static void ev_client_paired(void *ctx, const char *key, const char *name) {
    (void) ctx;
    JNIEnv *env = get_env();
    if (!env) {
        return;
    }
    jbyteArray k = utf8_bytes(env, key);
    jbyteArray n = utf8_bytes(env, name);
    (*env)->CallStaticVoidMethod(env, g_bridge, g_cb.client_paired, k, n);
    check_exception(env);
    (*env)->DeleteLocalRef(env, k);
    (*env)->DeleteLocalRef(env, n);
}

static void ev_track_info(void *ctx, const char *title, const char *artist, const char *album) {
    (void) ctx;
    JNIEnv *env = get_env();
    if (!env) {
        return;
    }
    jbyteArray t = utf8_bytes(env, title);
    jbyteArray a = utf8_bytes(env, artist);
    jbyteArray l = utf8_bytes(env, album);
    (*env)->CallStaticVoidMethod(env, g_bridge, g_cb.track_info, t, a, l);
    check_exception(env);
    delete_local(env, t);
    delete_local(env, a);
    delete_local(env, l);
}

static void ev_artwork(void *ctx, const uint8_t *data, size_t len) {
    (void) ctx;
    JNIEnv *env = get_env();
    if (!env) {
        return;
    }
    jbyteArray image = byte_array(env, data, len);
    if (data && !image) {
        return; /* too large to hand over: keep the cover that is shown */
    }
    (*env)->CallStaticVoidMethod(env, g_bridge, g_cb.artwork, image);
    check_exception(env);
    delete_local(env, image);
}

static void ev_progress(void *ctx, uint32_t start, uint32_t current, uint32_t end) {
    (void) ctx;
    JNIEnv *env = get_env();
    if (env) {
        (*env)->CallStaticVoidMethod(env, g_bridge, g_cb.progress, (jlong) start, (jlong) current, (jlong) end);
        check_exception(env);
    }
}

static void ev_remote_control(void *ctx, const char *dacp_id, const char *active_remote) {
    (void) ctx;
    JNIEnv *env = get_env();
    if (!env) {
        return;
    }
    jbyteArray d = utf8_bytes(env, dacp_id);
    jbyteArray a = utf8_bytes(env, active_remote);
    (*env)->CallStaticVoidMethod(env, g_bridge, g_cb.remote_control, d, a);
    check_exception(env);
    delete_local(env, d);
    delete_local(env, a);
}

static void ev_photo(void *ctx, const uint8_t *data, size_t len) {
    (void) ctx;
    JNIEnv *env = get_env();
    if (!env) {
        return;
    }
    jbyteArray image = byte_array(env, data, len);
    if (!image) {
        return;
    }
    (*env)->CallStaticVoidMethod(env, g_bridge, g_cb.photo, image);
    check_exception(env);
    delete_local(env, image);
}

static void ev_photo_stop(void *ctx) {
    (void) ctx;
    call_void(g_cb.photo_stop);
}

/* ------------------------------------------------------------------------- */
/* media sink wiring                                                          */

static void m_video_start(void *ctx) {
    (void) ctx;
    vd_start();
}

static void m_video_config(void *ctx, video_codec_t codec, const uint8_t *config, size_t len, int w, int h) {
    (void) ctx;
    vd_config(codec, config, len, w, h);
}

static void m_video_frame(void *ctx, const uint8_t *au, size_t len, uint64_t remote_ts_ns, bool keyframe) {
    (void) ctx;
    vd_frame(au, len, remote_ts_ns, keyframe);
}

static void m_video_suspend(void *ctx, bool suspended) {
    (void) ctx;
    vd_suspend(suspended);
}

static void m_video_stop(void *ctx) {
    (void) ctx;
    vd_stop();
}

static bool m_audio_start(void *ctx, const audio_format_t *format) {
    (void) ctx;
    return ap_start(format);
}

static void m_audio_frame(void *ctx, const uint8_t *data, size_t len, uint32_t rtp_ts, uint64_t remote_ts_ns) {
    (void) ctx;
    ap_frame(data, len, rtp_ts, remote_ts_ns);
}

static void m_audio_flush(void *ctx) {
    (void) ctx;
    ap_flush();
}

static void m_audio_volume(void *ctx, float db) {
    (void) ctx;
    ap_volume(db);
}

static void m_audio_stop(void *ctx) {
    (void) ctx;
    ap_stop();
}

static const media_sink_ops_t kMediaOps = {
    .video_start = m_video_start,
    .video_config = m_video_config,
    .video_frame = m_video_frame,
    .video_suspend = m_video_suspend,
    .video_stop = m_video_stop,
    .audio_start = m_audio_start,
    .audio_frame = m_audio_frame,
    .audio_flush = m_audio_flush,
    .audio_volume = m_audio_volume,
    .audio_stop = m_audio_stop,
};

/* ------------------------------------------------------------------------- */
/* native methods                                                             */

static void JNICALL n_set_log_level(JNIEnv *env, jclass cls, jint level) {
    (void) env;
    (void) cls;
    log_set_level((log_level_t) (level < 0 ? 0 : (level > LOGL_TRACE ? LOGL_TRACE : level)));
}

static void JNICALL n_log(JNIEnv *env, jclass cls, jint level, jint category, jstring message) {
    (void) cls;
    if (!message || category < 0 || category >= LOGC_COUNT) {
        return;
    }
    const char *m = (*env)->GetStringUTFChars(env, message, NULL);
    if (m) {
        log_history_add((log_level_t) (level < 0 ? 0 : (level > LOGL_TRACE ? LOGL_TRACE : level)),
                        (log_category_t) category, m);
        (*env)->ReleaseStringUTFChars(env, message, m);
    }
}

static bool copy_bytes(JNIEnv *env, jbyteArray arr, uint8_t *out, size_t len) {
    if (!arr || (*env)->GetArrayLength(env, arr) != (jsize) len) {
        return false;
    }
    (*env)->GetByteArrayRegion(env, arr, 0, (jsize) len, (jbyte *) out);
    return true;
}

static jint JNICALL n_start(JNIEnv *env, jclass cls, jbyteArray name, jbyteArray device_id, jstring public_id,
                            jbyteArray seed, jboolean require_pin, jint port, jint width, jint height, jint fps,
                            jboolean hevc) {
    (void) cls;
    airplay_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (name) {
        jsize n = (*env)->GetArrayLength(env, name);
        char raw[256];
        if (n > (jsize) sizeof(raw) - 1) {
            n = (jsize) sizeof(raw) - 1;
        }
        (*env)->GetByteArrayRegion(env, name, 0, n, (jbyte *) raw);
        sanitize_utf8(cfg.name, sizeof(cfg.name), raw, (size_t) n);
    }
    if (!copy_bytes(env, device_id, cfg.device_id, sizeof(cfg.device_id)) ||
        !copy_bytes(env, seed, cfg.identity_seed, sizeof(cfg.identity_seed))) {
        return -1;
    }
    if (public_id) {
        const char *pi = (*env)->GetStringUTFChars(env, public_id, NULL);
        if (pi) {
            str_copy(cfg.public_id, sizeof(cfg.public_id), pi);
            (*env)->ReleaseStringUTFChars(env, public_id, pi);
        }
    }
    cfg.require_pin = require_pin;
    cfg.port = (uint16_t) (port > 0 && port < 65536 ? port : 0);
    cfg.display_width = width;
    cfg.display_height = height;
    cfg.display_fps = fps;
    cfg.hevc = hevc;

    airplay_events_t ev = {
        .ctx = NULL,
        .session_started = ev_session_started,
        .session_ended = ev_session_ended,
        .pin_display = ev_pin,
        .client_paired = ev_client_paired,
        .track_info = ev_track_info,
        .artwork = ev_artwork,
        .progress = ev_progress,
        .remote_control = ev_remote_control,
        .photo = ev_photo,
        .photo_stop = ev_photo_stop,
    };

    pthread_mutex_lock(&g_server_lock);
    if (g_server) {
        airplay_server_destroy(g_server);
        g_server = NULL;
    }
    g_server = airplay_server_create(&cfg, &ev, &kMediaOps, NULL);
    secure_zero(cfg.identity_seed, sizeof(cfg.identity_seed));
    int bound = g_server ? airplay_server_start(g_server) : -1;
    if (bound < 0 && g_server) {
        airplay_server_destroy(g_server);
        g_server = NULL;
    }
    pthread_mutex_unlock(&g_server_lock);
    return bound;
}

static void JNICALL n_stop(JNIEnv *env, jclass cls) {
    (void) env;
    (void) cls;
    pthread_mutex_lock(&g_server_lock);
    airplay_server_t *s = g_server;
    g_server = NULL;
    pthread_mutex_unlock(&g_server_lock);
    if (s) {
        airplay_server_destroy(s);
        LOG_I(SERVICE, "receiver stopped");
    }
}

static jstring JNICALL n_public_key(JNIEnv *env, jclass cls) {
    (void) cls;
    char hex[65] = { 0 };
    pthread_mutex_lock(&g_server_lock);
    if (g_server) {
        airplay_public_key_hex(g_server, hex);
    }
    pthread_mutex_unlock(&g_server_lock);
    return hex[0] ? (*env)->NewStringUTF(env, hex) : NULL;
}

static jobjectArray JNICALL n_txt_records(JNIEnv *env, jclass cls, jboolean raop) {
    (void) cls;
    txt_entry_t entries[24];
    int n = 0;
    pthread_mutex_lock(&g_server_lock);
    if (g_server) {
        n = raop ? airplay_txt_raop(g_server, entries, 24) : airplay_txt_airplay(g_server, entries, 24);
    }
    pthread_mutex_unlock(&g_server_lock);
    if (n <= 0) {
        return NULL;
    }
    jclass string_class = (*env)->FindClass(env, "java/lang/String");
    jobjectArray arr = (*env)->NewObjectArray(env, n * 2, string_class, NULL);
    for (int i = 0; arr && i < n; i++) {
        jstring k = (*env)->NewStringUTF(env, entries[i].key);
        jstring v = (*env)->NewStringUTF(env, entries[i].value);
        (*env)->SetObjectArrayElement(env, arr, 2 * i, k);
        (*env)->SetObjectArrayElement(env, arr, 2 * i + 1, v);
        (*env)->DeleteLocalRef(env, k);
        (*env)->DeleteLocalRef(env, v);
    }
    return arr;
}

static void JNICALL n_set_paired_clients(JNIEnv *env, jclass cls, jobjectArray keys) {
    (void) cls;
    int n = keys ? (*env)->GetArrayLength(env, keys) : 0;
    if (n > 256) {
        n = 256;
    }
    char **list = n > 0 ? (char **) calloc((size_t) n, sizeof(char *)) : NULL;
    int count = 0;
    for (int i = 0; i < n && list; i++) {
        jstring s = (jstring) (*env)->GetObjectArrayElement(env, keys, i);
        if (!s) {
            continue;
        }
        const char *c = (*env)->GetStringUTFChars(env, s, NULL);
        if (c) {
            list[count] = strdup(c);
            if (list[count]) {
                count++;
            }
            (*env)->ReleaseStringUTFChars(env, s, c);
        }
        (*env)->DeleteLocalRef(env, s);
    }
    pthread_mutex_lock(&g_server_lock);
    if (g_server) {
        airplay_server_set_paired_clients(g_server, (const char *const *) list, count);
    }
    pthread_mutex_unlock(&g_server_lock);
    for (int i = 0; i < count; i++) {
        free(list[i]);
    }
    free(list);
}

static void JNICALL n_disconnect(JNIEnv *env, jclass cls) {
    (void) env;
    (void) cls;
    pthread_mutex_lock(&g_server_lock);
    if (g_server) {
        airplay_server_disconnect(g_server);
    }
    pthread_mutex_unlock(&g_server_lock);
}

static void JNICALL n_set_surface(JNIEnv *env, jclass cls, jobject surface) {
    (void) cls;
    ANativeWindow *window = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
    vd_set_display(window);
    if (window) {
        ANativeWindow_release(window); /* vd_set_display took its own reference */
    }
}

static void JNICALL n_set_decoder_preferences(JNIEnv *env, jclass cls, jstring avc, jstring hevc, jint options) {
    (void) cls;
    const char *a = avc ? (*env)->GetStringUTFChars(env, avc, NULL) : NULL;
    const char *h = hevc ? (*env)->GetStringUTFChars(env, hevc, NULL) : NULL;
    vd_set_preferences(a, h, options, 0, 0);
    if (a) {
        (*env)->ReleaseStringUTFChars(env, avc, a);
    }
    if (h) {
        (*env)->ReleaseStringUTFChars(env, hevc, h);
    }
}

static jint JNICALL n_read_audio(JNIEnv *env, jclass cls, jobject buffer, jint max_bytes, jint timeout_ms) {
    (void) cls;
    uint8_t *dst = (uint8_t *) (*env)->GetDirectBufferAddress(env, buffer);
    jlong cap = (*env)->GetDirectBufferCapacity(env, buffer);
    if (!dst || cap <= 0) {
        return -1;
    }
    if (max_bytes > cap) {
        max_bytes = (jint) cap;
    }
    return ap_read(dst, max_bytes, timeout_ms);
}

static void JNICALL n_stats(JNIEnv *env, jclass cls, jlongArray out) {
    (void) cls;
    jlong v[25];
    v[0] = (jlong) atomic_load(&g_stats.video_frames_in);
    v[1] = (jlong) atomic_load(&g_stats.video_bytes_in);
    v[2] = (jlong) atomic_load(&g_stats.video_keyframes_in);
    v[3] = (jlong) atomic_load(&g_stats.video_frames_decoded);
    v[4] = (jlong) atomic_load(&g_stats.video_frames_rendered);
    v[5] = (jlong) atomic_load(&g_stats.video_frames_dropped);
    v[6] = (jlong) atomic_load(&g_stats.video_decode_time_us_total);
    v[7] = (jlong) atomic_load(&g_stats.video_decode_samples);
    v[8] = (jlong) atomic_load(&g_stats.video_delay_us_total);
    v[9] = (jlong) atomic_load(&g_stats.video_delay_samples);
    v[10] = (jlong) atomic_load(&g_stats.video_width);
    v[11] = (jlong) atomic_load(&g_stats.video_height);
    v[12] = (jlong) atomic_load(&g_stats.video_codec);
    v[13] = (jlong) atomic_load(&g_stats.video_decoder_resets);
    v[14] = (jlong) atomic_load(&g_stats.audio_packets_in);
    v[15] = (jlong) atomic_load(&g_stats.audio_packets_lost);
    v[16] = (jlong) atomic_load(&g_stats.audio_frames_decoded);
    v[17] = (jlong) atomic_load(&g_stats.audio_underruns);
    v[18] = (jlong) atomic_load(&g_stats.audio_frames_dropped);
    v[19] = (jlong) atomic_load(&g_stats.audio_ct);
    v[20] = (jlong) atomic_load(&g_stats.audio_buffer_ms);
    v[21] = (jlong) atomic_load(&g_stats.clock_offset_us);
    v[22] = (jlong) atomic_load(&g_stats.clock_synced);
    v[23] = (jlong) atomic_load(&g_stats.sessions_started);
    v[24] = (jlong) atomic_load(&g_stats.connections_rejected);
    jsize n = (*env)->GetArrayLength(env, out);
    (*env)->SetLongArrayRegion(env, out, 0, n < 25 ? n : 25, v);
}

static jstring JNICALL n_log_history(JNIEnv *env, jclass cls) {
    (void) cls;
    size_t cap = 64 * 1024;
    char *buf = (char *) malloc(cap);
    if (!buf) {
        return NULL;
    }
    log_dump_history(buf, cap);
    /* keep the text ASCII-safe for NewStringUTF */
    for (char *p = buf; *p; p++) {
        if ((unsigned char) *p >= 0x80) {
            *p = '?';
        }
    }
    jstring s = (*env)->NewStringUTF(env, buf);
    free(buf);
    return s;
}

static jboolean JNICALL n_session_active(JNIEnv *env, jclass cls) {
    (void) env;
    (void) cls;
    pthread_mutex_lock(&g_server_lock);
    bool active = airplay_server_session_active(g_server);
    pthread_mutex_unlock(&g_server_lock);
    return (jboolean) active;
}

static const JNINativeMethod kMethods[] = {
    { "nativeSetLogLevel", "(I)V", (void *) n_set_log_level },
    { "nativeLog", "(IILjava/lang/String;)V", (void *) n_log },
    { "nativeStart", "([B[BLjava/lang/String;[BZIIIIZ)I", (void *) n_start },
    { "nativeStop", "()V", (void *) n_stop },
    { "nativePublicKey", "()Ljava/lang/String;", (void *) n_public_key },
    { "nativeTxtRecords", "(Z)[Ljava/lang/String;", (void *) n_txt_records },
    { "nativeSetPairedClients", "([Ljava/lang/String;)V", (void *) n_set_paired_clients },
    { "nativeDisconnect", "()V", (void *) n_disconnect },
    { "nativeSetSurface", "(Landroid/view/Surface;)V", (void *) n_set_surface },
    { "nativeSetDecoderPreferences", "(Ljava/lang/String;Ljava/lang/String;I)V", (void *) n_set_decoder_preferences },
    { "nativeReadAudio", "(Ljava/nio/ByteBuffer;II)I", (void *) n_read_audio },
    { "nativeStats", "([J)V", (void *) n_stats },
    { "nativeLogHistory", "()Ljava/lang/String;", (void *) n_log_history },
    { "nativeSessionActive", "()Z", (void *) n_session_active },
};

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void) reserved;
    JNIEnv *env = NULL;
    if ((*vm)->GetEnv(vm, (void **) &env, JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }
    g_vm = vm;
    pthread_key_create(&g_env_key, detach_thread);
    log_set_sink(android_log_sink);

    jclass local = (*env)->FindClass(env, BRIDGE_CLASS);
    if (!local) {
        return JNI_ERR;
    }
    g_bridge = (jclass) (*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);
    if ((*env)->RegisterNatives(env, g_bridge, kMethods, sizeof(kMethods) / sizeof(kMethods[0])) != 0) {
        return JNI_ERR;
    }
    g_cb.session_started = (*env)->GetStaticMethodID(env, g_bridge, "onSessionStarted", "([B[B)V");
    g_cb.session_ended = (*env)->GetStaticMethodID(env, g_bridge, "onSessionEnded", "()V");
    g_cb.pin = (*env)->GetStaticMethodID(env, g_bridge, "onPin", "([B)V");
    g_cb.client_paired = (*env)->GetStaticMethodID(env, g_bridge, "onClientPaired", "([B[B)V");
    g_cb.video_started = (*env)->GetStaticMethodID(env, g_bridge, "onVideoStarted", "()V");
    g_cb.video_stopped = (*env)->GetStaticMethodID(env, g_bridge, "onVideoStopped", "()V");
    g_cb.video_size = (*env)->GetStaticMethodID(env, g_bridge, "onVideoSize", "(II)V");
    g_cb.audio_started = (*env)->GetStaticMethodID(env, g_bridge, "onAudioStarted", "(IIZ)V");
    g_cb.audio_stopped = (*env)->GetStaticMethodID(env, g_bridge, "onAudioStopped", "()V");
    g_cb.volume = (*env)->GetStaticMethodID(env, g_bridge, "onVolume", "(F)V");
    g_cb.track_info = (*env)->GetStaticMethodID(env, g_bridge, "onTrackInfo", "([B[B[B)V");
    g_cb.artwork = (*env)->GetStaticMethodID(env, g_bridge, "onArtwork", "([B)V");
    g_cb.progress = (*env)->GetStaticMethodID(env, g_bridge, "onProgress", "(JJJ)V");
    g_cb.remote_control = (*env)->GetStaticMethodID(env, g_bridge, "onRemoteControl", "([B[B)V");
    g_cb.photo = (*env)->GetStaticMethodID(env, g_bridge, "onPhoto", "([B)V");
    g_cb.photo_stop = (*env)->GetStaticMethodID(env, g_bridge, "onPhotoStop", "()V");
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return JNI_ERR;
    }
    vd_init();
    return JNI_VERSION_1_6;
}
