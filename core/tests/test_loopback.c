/*
 * AirPlay TV - AirPlay screen mirroring receiver for Android TV
 * Copyright (C) 2026 besliky
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

/* End-to-end sessions over loopback sockets: a fake sender against the real server. */

#include <arpa/inet.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "airplay.h"
#include "fake_sender.h"
#include "h264_gen.h"
#include "mirror.h"
#include "ntp.h"
#include "test.h"
#include "util.h"

#define MAX_ITEMS 64

typedef struct {
    pthread_mutex_t lock;
    int video_starts;
    int video_stops;
    int configs;
    uint8_t config[4096];
    size_t config_len;
    int width;
    int height;
    int frames;
    uint8_t *frame[MAX_ITEMS];
    size_t frame_len[MAX_ITEMS];
    bool keyframe[MAX_ITEMS];
    int suspends;
    int audio_starts;
    int audio_stops;
    audio_format_t format;
    int audio_frames;
    uint8_t *audio[MAX_ITEMS];
    size_t audio_len[MAX_ITEMS];
    float volume;
    int sessions_started;
    int sessions_ended;
    char client_name[64];
    char pin[8];
    int paired;
    char paired_key[64];
    int tracks;
    char title[300];
    char artist[64];
    char album[64];
    int artworks;
    size_t artwork_len;
    uint8_t artwork_last;
    int progresses;
    uint32_t progress[3];
    int remotes;
    char dacp_id[32];
    char active_remote[32];
    int photos;
    size_t photo_len;
    uint8_t photo_last;
    int photo_stops;
} capture_t;

static capture_t g_cap;

static void cap_reset(void) {
    pthread_mutex_lock(&g_cap.lock);
    for (int i = 0; i < g_cap.frames && i < MAX_ITEMS; i++) free(g_cap.frame[i]);
    for (int i = 0; i < g_cap.audio_frames && i < MAX_ITEMS; i++) free(g_cap.audio[i]);
    pthread_mutex_t lock = g_cap.lock;
    memset(&g_cap, 0, sizeof(g_cap));
    g_cap.lock = lock;
    g_cap.volume = 99.0f;
    pthread_mutex_unlock(&g_cap.lock);
}

static void m_video_start(void *ctx) { (void) ctx; pthread_mutex_lock(&g_cap.lock); g_cap.video_starts++; pthread_mutex_unlock(&g_cap.lock); }
static void m_video_stop(void *ctx) { (void) ctx; pthread_mutex_lock(&g_cap.lock); g_cap.video_stops++; pthread_mutex_unlock(&g_cap.lock); }
static void m_video_suspend(void *ctx, bool s) { (void) ctx; (void) s; pthread_mutex_lock(&g_cap.lock); g_cap.suspends++; pthread_mutex_unlock(&g_cap.lock); }

static void m_video_config(void *ctx, video_codec_t codec, const uint8_t *config, size_t len, int w, int h) {
    (void) ctx;
    (void) codec;
    pthread_mutex_lock(&g_cap.lock);
    g_cap.configs++;
    if (len <= sizeof(g_cap.config)) {
        memcpy(g_cap.config, config, len);
        g_cap.config_len = len;
    }
    g_cap.width = w;
    g_cap.height = h;
    pthread_mutex_unlock(&g_cap.lock);
}

static void m_video_frame(void *ctx, const uint8_t *au, size_t len, uint64_t ts, bool key) {
    (void) ctx;
    (void) ts;
    pthread_mutex_lock(&g_cap.lock);
    if (g_cap.frames < MAX_ITEMS) {
        g_cap.frame[g_cap.frames] = (uint8_t *) malloc(len);
        memcpy(g_cap.frame[g_cap.frames], au, len);
        g_cap.frame_len[g_cap.frames] = len;
        g_cap.keyframe[g_cap.frames] = key;
    }
    g_cap.frames++;
    pthread_mutex_unlock(&g_cap.lock);
}

static bool m_audio_start(void *ctx, const audio_format_t *f) {
    (void) ctx;
    pthread_mutex_lock(&g_cap.lock);
    g_cap.audio_starts++;
    g_cap.format = *f;
    pthread_mutex_unlock(&g_cap.lock);
    return true;
}

static void m_audio_frame(void *ctx, const uint8_t *data, size_t len, uint32_t rtp, uint64_t ts) {
    (void) ctx;
    (void) rtp;
    (void) ts;
    pthread_mutex_lock(&g_cap.lock);
    if (g_cap.audio_frames < MAX_ITEMS) {
        g_cap.audio[g_cap.audio_frames] = (uint8_t *) malloc(len);
        memcpy(g_cap.audio[g_cap.audio_frames], data, len);
        g_cap.audio_len[g_cap.audio_frames] = len;
    }
    g_cap.audio_frames++;
    pthread_mutex_unlock(&g_cap.lock);
}

static void m_audio_flush(void *ctx) { (void) ctx; }
static void m_audio_volume(void *ctx, float db) { (void) ctx; pthread_mutex_lock(&g_cap.lock); g_cap.volume = db; pthread_mutex_unlock(&g_cap.lock); }
static void m_audio_stop(void *ctx) { (void) ctx; pthread_mutex_lock(&g_cap.lock); g_cap.audio_stops++; pthread_mutex_unlock(&g_cap.lock); }

static const media_sink_ops_t kOps = {
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

static void ev_started(void *ctx, const char *name, const char *model) {
    (void) ctx;
    (void) model;
    pthread_mutex_lock(&g_cap.lock);
    g_cap.sessions_started++;
    str_copy(g_cap.client_name, sizeof(g_cap.client_name), name);
    pthread_mutex_unlock(&g_cap.lock);
}

static void ev_ended(void *ctx) { (void) ctx; pthread_mutex_lock(&g_cap.lock); g_cap.sessions_ended++; pthread_mutex_unlock(&g_cap.lock); }

static void ev_pin(void *ctx, const char *pin) {
    (void) ctx;
    pthread_mutex_lock(&g_cap.lock);
    str_copy(g_cap.pin, sizeof(g_cap.pin), pin ? pin : "");
    pthread_mutex_unlock(&g_cap.lock);
}

static void ev_paired(void *ctx, const char *key, const char *name) {
    (void) ctx;
    (void) name;
    pthread_mutex_lock(&g_cap.lock);
    g_cap.paired++;
    str_copy(g_cap.paired_key, sizeof(g_cap.paired_key), key);
    pthread_mutex_unlock(&g_cap.lock);
}

static void ev_track_info(void *ctx, const char *title, const char *artist, const char *album) {
    (void) ctx;
    pthread_mutex_lock(&g_cap.lock);
    str_copy(g_cap.title, sizeof(g_cap.title), title);
    str_copy(g_cap.artist, sizeof(g_cap.artist), artist);
    str_copy(g_cap.album, sizeof(g_cap.album), album);
    g_cap.tracks++;
    pthread_mutex_unlock(&g_cap.lock);
}

static void ev_artwork(void *ctx, const uint8_t *data, size_t len) {
    (void) ctx;
    pthread_mutex_lock(&g_cap.lock);
    g_cap.artwork_len = data ? len : 0;
    g_cap.artwork_last = data && len ? data[len - 1] : 0;
    g_cap.artworks++;
    pthread_mutex_unlock(&g_cap.lock);
}

static void ev_progress(void *ctx, uint32_t start, uint32_t current, uint32_t end) {
    (void) ctx;
    pthread_mutex_lock(&g_cap.lock);
    g_cap.progress[0] = start;
    g_cap.progress[1] = current;
    g_cap.progress[2] = end;
    g_cap.progresses++;
    pthread_mutex_unlock(&g_cap.lock);
}

static void ev_remote_control(void *ctx, const char *dacp_id, const char *active_remote) {
    (void) ctx;
    pthread_mutex_lock(&g_cap.lock);
    str_copy(g_cap.dacp_id, sizeof(g_cap.dacp_id), dacp_id);
    str_copy(g_cap.active_remote, sizeof(g_cap.active_remote), active_remote);
    g_cap.remotes++;
    pthread_mutex_unlock(&g_cap.lock);
}

static void ev_photo(void *ctx, const uint8_t *data, size_t len) {
    (void) ctx;
    pthread_mutex_lock(&g_cap.lock);
    g_cap.photo_len = len;
    g_cap.photo_last = len ? data[len - 1] : 0;
    g_cap.photos++;
    pthread_mutex_unlock(&g_cap.lock);
}

static void ev_photo_stop(void *ctx) {
    (void) ctx;
    pthread_mutex_lock(&g_cap.lock);
    g_cap.photo_stops++;
    pthread_mutex_unlock(&g_cap.lock);
}

static const char *pin_source(void *ctx) {
    (void) ctx;
    static char pin[8];
    pthread_mutex_lock(&g_cap.lock);
    memcpy(pin, g_cap.pin, sizeof(pin));
    pthread_mutex_unlock(&g_cap.lock);
    return pin[0] ? pin : NULL;
}

static const char *wrong_pin_source(void *ctx) {
    const char *p = pin_source(ctx);
    static char wrong[8];
    if (!p) return NULL;
    str_copy(wrong, sizeof(wrong), p);
    wrong[0] = wrong[0] == '9' ? '0' : (char) (wrong[0] + 1);
    return wrong;
}

static airplay_server_t *start_server(bool require_pin, int *port) {
    cap_reset();
    airplay_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    str_copy(cfg.name, sizeof(cfg.name), "Test TV");
    memcpy(cfg.device_id, "\x02\x11\x22\x33\x44\x55", 6);
    str_copy(cfg.public_id, sizeof(cfg.public_id), "5d1a3c3e-0f2b-4c64-8a51-2d7c9f2b6a10");
    memset(cfg.identity_seed, 0x42, 32);
    cfg.require_pin = require_pin;
    cfg.port = 0;
    cfg.display_width = 1920;
    cfg.display_height = 1080;
    cfg.display_fps = 60;
    airplay_events_t ev = {
        .session_started = ev_started, .session_ended = ev_ended, .pin_display = ev_pin, .client_paired = ev_paired,
        .track_info = ev_track_info, .artwork = ev_artwork, .progress = ev_progress,
        .remote_control = ev_remote_control, .photo = ev_photo, .photo_stop = ev_photo_stop,
    };
    airplay_server_t *s = airplay_server_create(&cfg, &ev, &kOps, NULL);
    if (!s) return NULL;
    *port = airplay_server_start(s);
    if (*port <= 0) {
        airplay_server_destroy(s);
        return NULL;
    }
    return s;
}

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long) (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* Waits until *counter (read under the lock) reaches target. */
static bool wait_for(const int *counter, int target) {
    for (int i = 0; i < 300; i++) {
        pthread_mutex_lock(&g_cap.lock);
        int v = *counter;
        pthread_mutex_unlock(&g_cap.lock);
        if (v >= target) return true;
        sleep_ms(10);
    }
    return false;
}

static void run_mirroring(fake_sender_t *f, int frames) {
    h264_gen_t g;
    h264_gen_init(&g, 320, 192);
    uint8_t avcc[128];
    size_t avcc_len = h264_gen_avcc(&g, avcc, sizeof(avcc));
    fs_send_codec(f, avcc, avcc_len, 320, 192, 0x16);
    static uint8_t au[200000];
    for (int i = 0; i < frames; i++) {
        size_t n = h264_gen_frame(&g, i % 5 == 0, i, au, sizeof(au));
        fs_send_video(f, au, n, ns_to_ntp(time_mono_ns()));
    }
}

TEST(loopback_full_session_with_legacy_pairing) {
    int port = 0;
    airplay_server_t *s = start_server(false, &port);
    CHECK(s != NULL);
    fake_sender_t f;
    CHECK(fs_connect(&f, "127.0.0.1", (uint16_t) port) == 0);
    CHECK(fs_pair(&f) == 0);
    CHECK(fs_fairplay(&f) == 0);
    CHECK_EQ(fs_setup_session(&f, "Test iPhone", "iPhone15,2"), 200);
    CHECK(wait_for(&g_cap.sessions_started, 1));
    CHECK_STR(g_cap.client_name, "Test iPhone");
    CHECK(airplay_server_session_active(s));

    CHECK(fs_setup_mirror(&f, 0x1234567890abcdefULL) == 0);
    CHECK(wait_for(&g_cap.video_starts, 1));

    /* build the stream once more to know the expected Annex-B frames */
    h264_gen_t g;
    h264_gen_init(&g, 320, 192);
    uint8_t avcc[128];
    size_t avcc_len = h264_gen_avcc(&g, avcc, sizeof(avcc));
    static uint8_t expected[12][200000];
    size_t expected_len[12];
    for (int i = 0; i < 12; i++) {
        expected_len[i] = h264_gen_frame(&g, i % 5 == 0, i, expected[i], sizeof(expected[i]));
        bool key;
        mirror_avcc_to_annexb(expected[i], expected_len[i], VIDEO_CODEC_H264, &key);
    }
    run_mirroring(&f, 12);
    CHECK(wait_for(&g_cap.frames, 12));
    CHECK_EQ(g_cap.configs, 1);
    CHECK_EQ(g_cap.width, 320);
    CHECK_EQ(g_cap.height, 192);
    uint8_t config[256];
    video_codec_t codec;
    int clen = mirror_parse_codec_config(avcc, avcc_len, &codec, config, sizeof(config));
    CHECK_EQ(g_cap.config_len, clen);
    CHECK_MEM(g_cap.config, config, (size_t) clen);
    for (int i = 0; i < 12; i++) {
        CHECK_EQ(g_cap.frame_len[i], expected_len[i]);
        CHECK_MEM(g_cap.frame[i], expected[i], expected_len[i]);
        CHECK_EQ(g_cap.keyframe[i], i % 5 == 0);
    }

    /* sender paused and resumed the stream (phone locked and unlocked) */
    fs_send_codec(&f, avcc, avcc_len, 320, 192, 0x56);
    fs_send_codec(&f, avcc, avcc_len, 320, 192, 0x16);
    CHECK(wait_for(&g_cap.suspends, 2));

    /* audio: ALAC, encrypted per packet */
    CHECK(fs_setup_audio(&f, 2, 352) == 0);
    CHECK(wait_for(&g_cap.audio_starts, 1));
    CHECK_EQ(g_cap.format.ct, 2);
    CHECK_EQ(g_cap.format.samples_per_frame, 352);
    CHECK(fs_send_sync(&f) == 0);
    uint8_t frame[2048];
    uint8_t sent[4][2048];
    size_t sent_len[4];
    for (int i = 0; i < 4; i++) {
        size_t n = fs_make_alac_frame(frame, sizeof(frame), i);
        memcpy(sent[i], frame, n);
        sent_len[i] = n;
        CHECK(fs_send_audio(&f, frame, n, 352) == 0);
    }
    CHECK(wait_for(&g_cap.audio_frames, 4));
    for (int i = 0; i < 4; i++) {
        CHECK_EQ(g_cap.audio_len[i], sent_len[i]);
        CHECK_MEM(g_cap.audio[i], sent[i], sent_len[i]);
    }
    CHECK(fs_set_volume(&f, -12.5f) == 0);
    CHECK(g_cap.volume > -12.6f && g_cap.volume < -12.4f);
    CHECK(fs_feedback(&f) == 0);

    /* stop mirroring only, then the whole session */
    CHECK(fs_teardown(&f, 110) == 0);
    CHECK(wait_for(&g_cap.video_stops, 1));
    CHECK_EQ(g_cap.sessions_ended, 0);
    CHECK(fs_teardown(&f, 0) == 0);
    CHECK(wait_for(&g_cap.sessions_ended, 1));
    CHECK(wait_for(&g_cap.audio_stops, 1));
    CHECK(!airplay_server_session_active(s));
    fs_close(&f);
    airplay_server_destroy(s);
}

/* Writes one DMAP item (tag, big-endian length, payload) and returns its size. */
static size_t dmap_item(uint8_t *out, const char *tag, const void *data, size_t len) {
    memcpy(out, tag, 4);
    out[4] = (uint8_t) (len >> 24);
    out[5] = (uint8_t) (len >> 16);
    out[6] = (uint8_t) (len >> 8);
    out[7] = (uint8_t) len;
    if (len) memcpy(out + 8, data, len);
    return 8 + len;
}

static int read_count(const int *counter) {
    pthread_mutex_lock(&g_cap.lock);
    int v = *counter;
    pthread_mutex_unlock(&g_cap.lock);
    return v;
}

TEST(loopback_now_playing_metadata) {
    int port = 0;
    airplay_server_t *s = start_server(false, &port);
    CHECK(s != NULL);
    fake_sender_t f;
    CHECK(fs_connect(&f, "127.0.0.1", (uint16_t) port) == 0);
    snprintf(f.extra_headers, sizeof(f.extra_headers), "DACP-ID: 1A2B3C4D5E6F7081\r\nActive-Remote: 987654321\r\n");
    CHECK(fs_pair(&f) == 0);
    CHECK(fs_fairplay(&f) == 0);
    CHECK_EQ(fs_setup_session(&f, "Test iPhone", "iPhone15,2"), 200);
    CHECK(wait_for(&g_cap.sessions_started, 1));

    /* the remote-control identity comes with the session, once however many requests carry it */
    CHECK(wait_for(&g_cap.remotes, 1));
    CHECK_STR(g_cap.dacp_id, "1A2B3C4D5E6F7081");
    CHECK_STR(g_cap.active_remote, "987654321");

    uint8_t inner[256];
    size_t n = 0;
    n += dmap_item(inner + n, "minm", "Midnight City", 13);
    n += dmap_item(inner + n, "asar", "M83", 3);
    n += dmap_item(inner + n, "asal", "Hurry Up", 8);
    uint8_t body[300];
    size_t len = dmap_item(body, "mlit", inner, n);
    CHECK(fs_set_parameter(&f, "application/x-dmap-tagged", body, len) == 0);
    CHECK(wait_for(&g_cap.tracks, 1));
    CHECK_STR(g_cap.title, "Midnight City");
    CHECK_STR(g_cap.artist, "M83");
    CHECK_STR(g_cap.album, "Hurry Up");

    uint8_t art[1000];
    for (size_t i = 0; i < sizeof(art); i++) art[i] = (uint8_t) i;
    CHECK(fs_set_parameter(&f, "image/jpeg", art, sizeof(art)) == 0);
    CHECK(wait_for(&g_cap.artworks, 1));
    CHECK_EQ(g_cap.artwork_len, sizeof(art));
    CHECK_EQ(g_cap.artwork_last, (uint8_t) (sizeof(art) - 1));
    /* a track without a cover */
    CHECK(fs_set_parameter(&f, "image/none", NULL, 0) == 0);
    CHECK(wait_for(&g_cap.artworks, 2));
    CHECK_EQ(g_cap.artwork_len, 0);

    const char progress[] = "progress: 1000/45100/441000\r\n";
    CHECK(fs_set_parameter(&f, "text/parameters", progress, sizeof(progress) - 1) == 0);
    CHECK(wait_for(&g_cap.progresses, 1));
    CHECK_EQ(g_cap.progress[0], 1000);
    CHECK_EQ(g_cap.progress[1], 45100);
    CHECK_EQ(g_cap.progress[2], 441000);
    const char wrapped[] = "progress: 4294967000/4294967295/88000\r\n";
    CHECK(fs_set_parameter(&f, "text/parameters", wrapped, sizeof(wrapped) - 1) == 0);
    CHECK(wait_for(&g_cap.progresses, 2));
    CHECK(g_cap.progress[1] == 4294967295u);
    const char *bad[] = { "progress: nonsense\r\n", "progress: 1/2\r\n", "progress: 1/2/4294967296\r\n",
                          "progress: -1/2/3\r\n" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(fs_set_parameter(&f, "text/parameters", bad[i], strlen(bad[i])) == 0);
    }

    /* a new token is reported again; malformed identities are ignored */
    snprintf(f.extra_headers, sizeof(f.extra_headers), "DACP-ID: 1A2B3C4D5E6F7081\r\nActive-Remote: 42\r\n");
    CHECK(fs_feedback(&f) == 0);
    CHECK(wait_for(&g_cap.remotes, 2));
    CHECK_STR(g_cap.active_remote, "42");
    snprintf(f.extra_headers, sizeof(f.extra_headers), "DACP-ID: iTunes_Ctrl_x\r\nActive-Remote: 42\r\n");
    CHECK(fs_feedback(&f) == 0);

    /* another connection without the session cannot change what is shown */
    fake_sender_t g;
    CHECK(fs_connect(&g, "127.0.0.1", (uint16_t) port) == 0);
    CHECK(fs_set_parameter(&g, "application/x-dmap-tagged", body, len) == 0);
    CHECK(fs_set_parameter(&g, "text/parameters", progress, sizeof(progress) - 1) == 0);
    fs_close(&g);

    CHECK(fs_feedback(&f) == 0); /* everything before this has been handled */
    CHECK_EQ(read_count(&g_cap.tracks), 1);
    CHECK_EQ(read_count(&g_cap.progresses), 2);
    CHECK_EQ(read_count(&g_cap.remotes), 2);
    fs_close(&f);
    airplay_server_destroy(s);
}

/* A JPEG-looking payload: the start marker, a counting pattern and a distinct last byte. */
static uint8_t *fake_jpeg(size_t len, uint8_t last) {
    uint8_t *p = (uint8_t *) malloc(len);
    if (!p) return NULL;
    for (size_t i = 0; i < len; i++) p[i] = (uint8_t) (i * 7u);
    p[0] = 0xff;
    p[1] = 0xd8;
    p[2] = 0xff;
    p[len - 1] = last;
    return p;
}

TEST(loopback_photos) {
    int port = 0;
    airplay_server_t *s = start_server(false, &port);
    CHECK(s != NULL);
    char head[1024];
    uint8_t *body = NULL;
    size_t body_len = 0;

    /* the Photos app first asks what the receiver supports and opens its event channel */
    fake_sender_t events;
    CHECK(fs_connect(&events, "127.0.0.1", (uint16_t) port) == 0);
    CHECK_EQ(fs_http(&events, "GET", "/server-info", NULL, NULL, 0, NULL, 0, &body, &body_len), 200);
    CHECK(body != NULL && body_len > 0);
    char *xml = (char *) calloc(1, body_len + 1);
    CHECK(xml != NULL);
    memcpy(xml, body, body_len);
    free(body);
    bool has_id = strstr(xml, "<key>deviceid</key><string>02:11:22:33:44:55</string>") != NULL;
    free(xml);
    CHECK(has_id);
    CHECK_EQ(fs_http(&events, "POST", "/reverse", "Upgrade: PTTH/1.0\r\nConnection: Upgrade\r\nX-Apple-Purpose: event\r\n",
                     NULL, 0, head, sizeof(head), NULL, NULL), 101);
    CHECK(strstr(head, "Upgrade: PTTH/1.0") != NULL);

    fake_sender_t f;
    CHECK(fs_connect(&f, "127.0.0.1", (uint16_t) port) == 0);
    uint8_t *one = fake_jpeg(3000, 0xd9);
    CHECK(one != NULL);
    int status = fs_http(&f, "PUT", "/photo", "X-Apple-AssetKey: one\r\nX-Apple-Transition: Dissolve\r\n", one, 3000,
                         head, sizeof(head), NULL, NULL);
    free(one);
    CHECK_EQ(status, 200);
    CHECK(strstr(head, "Content-Length: 0") != NULL);
    CHECK(wait_for(&g_cap.photos, 1));
    CHECK_EQ(g_cap.photo_len, 3000);
    CHECK_EQ(g_cap.photo_last, 0xd9);

    /* cacheOnly keeps a photo without showing it; displayCached shows it later */
    uint8_t *two = fake_jpeg(5000, 0x22);
    CHECK(two != NULL);
    status = fs_http(&f, "PUT", "/photo", "X-Apple-AssetKey: two\r\nX-Apple-AssetAction: cacheOnly\r\n", two, 5000,
                     NULL, 0, NULL, NULL);
    free(two);
    CHECK_EQ(status, 200);
    CHECK_EQ(read_count(&g_cap.photos), 1);
    CHECK_EQ(fs_http(&f, "PUT", "/photo", "X-Apple-AssetKey: two\r\nX-Apple-AssetAction: displayCached\r\n", NULL, 0,
                     NULL, 0, NULL, NULL), 200);
    CHECK(wait_for(&g_cap.photos, 2));
    CHECK_EQ(g_cap.photo_len, 5000);
    CHECK_EQ(g_cap.photo_last, 0x22);
    /* the photo shown first was kept as well */
    CHECK_EQ(fs_http(&f, "PUT", "/photo", "X-Apple-AssetKey: one\r\nX-Apple-AssetAction: displayCached\r\n", NULL, 0,
                     NULL, 0, NULL, NULL), 200);
    CHECK(wait_for(&g_cap.photos, 3));
    CHECK_EQ(g_cap.photo_last, 0xd9);

    /* an unknown key makes the sender send the photo again; a body that is no image is refused */
    CHECK_EQ(fs_http(&f, "PUT", "/photo", "X-Apple-AssetKey: never\r\nX-Apple-AssetAction: displayCached\r\n", NULL,
                     0, NULL, 0, NULL, NULL), 412);
    uint8_t junk[64];
    memset(junk, 'x', sizeof(junk));
    CHECK_EQ(fs_http(&f, "PUT", "/photo", "X-Apple-AssetKey: junk\r\n", junk, sizeof(junk), NULL, 0, NULL, NULL), 400);
    CHECK_EQ(read_count(&g_cap.photos), 3);

    /* a full-size photo above the general body limit */
    size_t big_len = 5u * 1024u * 1024u;
    uint8_t *big = fake_jpeg(big_len, 0x77);
    CHECK(big != NULL);
    status = fs_http(&f, "PUT", "/photo", "X-Apple-AssetKey: big\r\n", big, big_len, NULL, 0, NULL, NULL);
    free(big);
    CHECK_EQ(status, 200);
    CHECK(wait_for(&g_cap.photos, 4));
    CHECK_EQ(g_cap.photo_len, big_len);
    CHECK_EQ(g_cap.photo_last, 0x77);

    CHECK_EQ(fs_http(&f, "POST", "/stop", NULL, NULL, 0, NULL, 0, NULL, NULL), 200);
    CHECK(wait_for(&g_cap.photo_stops, 1));
    /* the cache went with the session */
    CHECK_EQ(fs_http(&f, "PUT", "/photo", "X-Apple-AssetKey: two\r\nX-Apple-AssetAction: displayCached\r\n", NULL, 0,
                     NULL, 0, NULL, NULL), 412);

    /* AirPlay video and everything else over HTTP is refused */
    fake_sender_t h;
    CHECK(fs_connect(&h, "127.0.0.1", (uint16_t) port) == 0);
    CHECK_EQ(fs_http(&h, "POST", "/play", NULL, "x", 1, NULL, 0, NULL, NULL), 501);
    fs_close(&h);
    fs_close(&f);
    fs_close(&events);
    airplay_server_destroy(s);
}

TEST(loopback_closing_the_event_channel_ends_the_photo_session) {
    int port = 0;
    airplay_server_t *s = start_server(false, &port);
    CHECK(s != NULL);
    fake_sender_t events;
    CHECK(fs_connect(&events, "127.0.0.1", (uint16_t) port) == 0);
    CHECK_EQ(fs_http(&events, "POST", "/reverse", "Upgrade: PTTH/1.0\r\nConnection: Upgrade\r\n", NULL, 0, NULL, 0,
                     NULL, NULL), 101);
    fake_sender_t f;
    CHECK(fs_connect(&f, "127.0.0.1", (uint16_t) port) == 0);
    uint8_t *jpeg = fake_jpeg(1000, 0x11);
    CHECK(jpeg != NULL);
    int status = fs_http(&f, "PUT", "/photo", "X-Apple-AssetKey: k\r\n", jpeg, 1000, NULL, 0, NULL, NULL);
    free(jpeg);
    CHECK_EQ(status, 200);
    CHECK(wait_for(&g_cap.photos, 1));
    CHECK_EQ(read_count(&g_cap.photo_stops), 0);
    fs_close(&events);
    CHECK(wait_for(&g_cap.photo_stops, 1));
    fs_close(&f);
    airplay_server_destroy(s);
}

TEST(loopback_photos_need_a_paired_sender_in_pin_mode) {
    int port = 0;
    airplay_server_t *s = start_server(true, &port);
    CHECK(s != NULL);
    fake_sender_t f;
    CHECK(fs_connect(&f, "127.0.0.1", (uint16_t) port) == 0);
    uint8_t *jpeg = fake_jpeg(1000, 0x11);
    CHECK(jpeg != NULL);
    int status = fs_http(&f, "PUT", "/photo", "X-Apple-AssetKey: k\r\n", jpeg, 1000, NULL, 0, NULL, NULL);
    free(jpeg);
    CHECK_EQ(status, 403);
    CHECK_EQ(read_count(&g_cap.photos), 0);
    fs_close(&f);
    airplay_server_destroy(s);
}

TEST(loopback_session_without_pairing) {
    int port = 0;
    airplay_server_t *s = start_server(false, &port);
    CHECK(s != NULL);
    fake_sender_t f;
    CHECK(fs_connect(&f, "127.0.0.1", (uint16_t) port) == 0);
    CHECK(fs_info(&f, NULL) == 0);
    CHECK(fs_fairplay(&f) == 0);
    CHECK_EQ(fs_setup_session(&f, "No Pairing", "iPad13,1"), 200);
    CHECK(fs_setup_mirror(&f, 77) == 0);
    run_mirroring(&f, 3);
    CHECK(wait_for(&g_cap.frames, 3));
    /* the sender disappears without TEARDOWN: the session must end */
    fs_close(&f);
    CHECK(wait_for(&g_cap.sessions_ended, 1));
    CHECK(wait_for(&g_cap.video_stops, 1));
    airplay_server_destroy(s);
}

TEST(loopback_setup_before_fairplay_is_refused) {
    int port = 0;
    airplay_server_t *s = start_server(false, &port);
    CHECK(s != NULL);
    fake_sender_t f;
    CHECK(fs_connect(&f, "127.0.0.1", (uint16_t) port) == 0);
    CHECK_EQ(fs_setup_session(&f, "x", "y"), 403);
    fs_close(&f);
    CHECK_EQ(g_cap.sessions_started, 0);
    airplay_server_destroy(s);
}

TEST(loopback_pin_pairing) {
    int port = 0;
    airplay_server_t *s = start_server(true, &port);
    CHECK(s != NULL);

    /* without pairing, SETUP is refused in PIN mode */
    fake_sender_t f;
    CHECK(fs_connect(&f, "127.0.0.1", (uint16_t) port) == 0);
    CHECK(fs_fairplay(&f) == 0);
    CHECK_EQ(fs_setup_session(&f, "Intruder", "iPhone"), 403);
    fs_close(&f);

    /* a wrong PIN fails */
    CHECK(fs_connect(&f, "127.0.0.1", (uint16_t) port) == 0);
    CHECK(fs_pair_pin(&f, wrong_pin_source, NULL) != 0);
    fs_close(&f);

    /* the right PIN pairs, the sender is remembered, the session starts */
    CHECK(fs_connect(&f, "127.0.0.1", (uint16_t) port) == 0);
    CHECK(fs_pair_pin(&f, pin_source, NULL) == 0);
    CHECK(fs_fairplay(&f) == 0);
    CHECK_EQ(fs_setup_session(&f, "Owner", "iPhone16,1"), 200);
    CHECK(wait_for(&g_cap.paired, 1));
    CHECK_EQ(strlen(g_cap.paired_key), 44);
    CHECK(wait_for(&g_cap.sessions_started, 1));
    CHECK(fs_teardown(&f, 0) == 0);
    uint8_t secret[64], pub[32];
    memcpy(secret, f.ed_secret, 64);
    memcpy(pub, f.ed_public, 32);
    fs_close(&f);

    /* the same sender comes back: pair-verify alone is enough */
    CHECK(fs_connect(&f, "127.0.0.1", (uint16_t) port) == 0);
    memcpy(f.ed_secret, secret, 64);
    memcpy(f.ed_public, pub, 32);
    CHECK(fs_pair(&f) == 0);
    CHECK(fs_fairplay(&f) == 0);
    CHECK_EQ(fs_setup_session(&f, "Owner", "iPhone16,1"), 200);
    fs_close(&f);
    airplay_server_destroy(s);
}

TEST(loopback_pin_hidden_when_sender_leaves) {
    int port = 0;
    airplay_server_t *s = start_server(true, &port);
    CHECK(s != NULL);
    fake_sender_t f;
    CHECK(fs_connect(&f, "127.0.0.1", (uint16_t) port) == 0);
    int status = 0;
    CHECK(fs_request(&f, "POST", "/pair-pin-start", NULL, NULL, 0, &status, NULL, NULL) == 0);
    CHECK_EQ(status, 200);
    CHECK(pin_source(NULL) != NULL);
    CHECK_EQ(strlen(g_cap.pin), 4);
    fs_close(&f);
    bool hidden = false;
    for (int i = 0; i < 300 && !hidden; i++) {
        pthread_mutex_lock(&g_cap.lock);
        hidden = g_cap.pin[0] == 0;
        pthread_mutex_unlock(&g_cap.lock);
        sleep_ms(10);
    }
    CHECK(hidden);
    airplay_server_destroy(s);
}

TEST(loopback_new_sender_takes_over) {
    int port = 0;
    airplay_server_t *s = start_server(false, &port);
    CHECK(s != NULL);
    fake_sender_t a;
    fake_sender_t b;
    CHECK(fs_connect(&a, "127.0.0.1", (uint16_t) port) == 0);
    CHECK(fs_fairplay(&a) == 0);
    CHECK_EQ(fs_setup_session(&a, "First", "iPhone"), 200);
    CHECK(fs_setup_mirror(&a, 1) == 0);
    CHECK(fs_connect(&b, "127.0.0.1", (uint16_t) port) == 0);
    CHECK(fs_fairplay(&b) == 0);
    CHECK_EQ(fs_setup_session(&b, "Second", "iPhone"), 200);
    CHECK(wait_for(&g_cap.sessions_started, 2));
    CHECK(wait_for(&g_cap.sessions_ended, 1));
    CHECK_STR(g_cap.client_name, "Second");
    CHECK(fs_teardown(&b, 0) == 0);
    fs_close(&a);
    fs_close(&b);
    airplay_server_destroy(s);
}

TEST(loopback_receiver_side_disconnect) {
    int port = 0;
    airplay_server_t *s = start_server(false, &port);
    CHECK(s != NULL);
    fake_sender_t f;
    CHECK(fs_connect(&f, "127.0.0.1", (uint16_t) port) == 0);
    CHECK(fs_fairplay(&f) == 0);
    CHECK_EQ(fs_setup_session(&f, "Phone", "iPhone"), 200);
    airplay_server_disconnect(s);
    CHECK(wait_for(&g_cap.sessions_ended, 1));
    fs_close(&f);
    airplay_server_destroy(s);
}

TEST(loopback_survives_hostile_clients) {
    int port = 0;
    airplay_server_t *s = start_server(false, &port);
    CHECK(s != NULL);
    const char *attacks[] = {
        "GET /info RTSP/1.0\r\nCSeq: -5\r\n\r\n",
        "POST /pair-verify RTSP/1.0\r\nCSeq: 1\r\nContent-Length: 3\r\n\r\n\x01\x00\x00",
        "POST /fp-setup RTSP/1.0\r\nCSeq: 2\r\nContent-Length: 16\r\n\r\nFPLY\x03\x01\x01\x00\x00\x00\x00\x04\x02\x00\xff\x00",
        "SETUP rtsp://x/1 RTSP/1.0\r\nCSeq: 3\r\nContent-Length: 8\r\n\r\nbplist00",
        "TEARDOWN rtsp://x/1 RTSP/1.0\r\nCSeq: 4\r\nContent-Length: 4\r\n\r\njunk",
        "GET /server-info HTTP/1.1\r\n\r\n",
        "\x16\x03\x01\x02\x00\x01\x00\x01\xfc\x03\x03",
        "OPTIONS * RTSP/1.0\r\nCSeq: 9\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof(attacks) / sizeof(attacks[0]); i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t) port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(connect(fd, (struct sockaddr *) &a, sizeof(a)) == 0);
        size_t len = strlen(attacks[i]);
        if (i == 1 || i == 2) {
            len = strstr(attacks[i], "\r\n\r\n") - attacks[i] + 4 + (i == 1 ? 3 : 16);
        }
        send(fd, attacks[i], len, MSG_NOSIGNAL);
        char buf[512];
        struct timeval tv = { 1, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        recv(fd, buf, sizeof(buf), 0);
        close(fd);
    }
    /* many half-open connections */
    int fds[20];
    for (int i = 0; i < 20; i++) {
        fds[i] = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t) port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        connect(fds[i], (struct sockaddr *) &a, sizeof(a));
        send(fds[i], "GET /info RTSP/1.0\r\n", 20, MSG_NOSIGNAL);
    }
    /* a legitimate sender still gets through */
    fake_sender_t f;
    CHECK(fs_connect(&f, "127.0.0.1", (uint16_t) port) == 0);
    CHECK(fs_info(&f, NULL) == 0);
    fs_close(&f);
    for (int i = 0; i < 20; i++) close(fds[i]);
    CHECK_EQ(g_cap.sessions_started, 0);
    airplay_server_destroy(s);
}

TEST(txt_records_match_protocol) {
    int port = 0;
    airplay_server_t *s = start_server(true, &port);
    CHECK(s != NULL);
    txt_entry_t e[24];
    int n = airplay_txt_airplay(s, e, 24);
    CHECK(n >= 9);
    bool have_features = false;
    bool have_pk = false;
    for (int i = 0; i < n; i++) {
        if (!strcmp(e[i].key, "features")) {
            have_features = true;
            CHECK_STR(e[i].value, "0x5A7FFEE6,0x0"); /* legacy pairing bit set for PIN mode */
        }
        if (!strcmp(e[i].key, "pk")) {
            have_pk = true;
            CHECK_EQ(strlen(e[i].value), 64);
        }
        if (!strcmp(e[i].key, "deviceid")) CHECK_STR(e[i].value, "02:11:22:33:44:55");
        if (!strcmp(e[i].key, "pw")) CHECK_STR(e[i].value, "true");
    }
    CHECK(have_features && have_pk);
    n = airplay_txt_raop(s, e, 24);
    CHECK(n >= 19);
    uint8_t wire[1024];
    size_t len = airplay_txt_encode(e, n, wire, sizeof(wire));
    CHECK(len > 100);
    CHECK_EQ(wire[0], strlen("ch=2"));
    char name[128];
    airplay_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    memcpy(cfg.device_id, "\x02\x11\x22\x33\x44\x55", 6);
    str_copy(cfg.name, sizeof(cfg.name), "Living Room");
    airplay_raop_name(&cfg, name, sizeof(name));
    CHECK_STR(name, "021122334455@Living Room");
    cfg.hevc = true;
    cfg.require_pin = false;
    CHECK(airplay_features(&cfg) == (0x527FFEE6ULL | (1ULL << 42)));
    airplay_server_destroy(s);
}
