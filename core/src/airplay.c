/*
 * AirPlay TV - AirPlay screen mirroring receiver for Android TV
 * Copyright (C) 2026 besliky
 *
 * AirPlay RTSP server and request handlers. The protocol behaviour (request
 * set, /info contents, SETUP key handling, stream setup and teardown) follows
 * UxPlay's raop.c and raop_handlers.h (Juho Vähä-Herttua, dsafa22,
 * F. Duncanh and contributors; LGPL-2.1-or-later), re-implemented around an
 * event-driven connection loop with strict validation of all input.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "airplay.h"

#include <errno.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "audio_rtp.h"
#include "bplist.h"
#include "crypto.h"
#include "dmap.h"
#include "fairplay.h"
#include "log.h"
#include "mirror.h"
#include "netutil.h"
#include "ntp.h"
#include "pairing.h"
#include "rtsp.h"
#include "util.h"

#define MAX_CONNECTIONS 8
#define RX_LIMIT ((size_t) RTSP_MAX_HEAD + RTSP_MAX_PUT_BODY)
#define IDLE_TIMEOUT_NS (60 * NS_PER_SEC)
#define PARTIAL_REQUEST_TIMEOUT_NS (15 * NS_PER_SEC)
#define SESSION_TIMEOUT_NS (90 * NS_PER_SEC)
#define PIN_TIMEOUT_NS (120 * NS_PER_SEC)
#define EVENT_CHANNEL_TIMEOUT_NS (6ull * 3600ull * NS_PER_SEC)

/* Photos the sender may ask to show again by their key (X-Apple-AssetAction: displayCached). */
#define PHOTO_CACHE_SLOTS 8
#define PHOTO_CACHE_BYTES ((size_t) 48 * 1024 * 1024)

#define CT_BPLIST "application/x-apple-binary-plist"
#define CT_OCTET "application/octet-stream"
#define CT_PARAMS "text/parameters"
#define CT_DMAP "application/x-dmap-tagged"

#define FEATURES_BASE 0x527FFEE6u     /* UxPlay's mirroring + audio feature set */
#define FEATURE_LEGACY_PAIRING (1u << 27)
#define FEATURE_SCREEN_MULTI_CODEC (1ull << 42)

typedef struct session session_t;
typedef struct conn conn_t;

struct conn {
    int fd;
    struct sockaddr_storage peer;
    uint8_t *rx;
    size_t rx_len;
    size_t rx_cap;
    uint64_t created_ns;
    uint64_t last_rx_ns;
    uint64_t partial_since_ns;
    bool rtsp;
    bool event_channel;       /* turned around by POST /reverse; idle for as long as a photo is shown */
    bool close_after_reply;
    bool closing;
    pairing_session_t pair;
    bool pair_trusted;        /* PIN mode: pair-verify proved a paired identity */
    fairplay_t fp;
    session_t *session;
};

struct session {
    airplay_server_t *server;
    conn_t *owner;
    uint32_t id;
    uint8_t key[16];
    uint8_t iv[16];
    ntp_client_t *ntp;
    mirror_receiver_t *mirror;
    audio_receiver_t *audio;
    char client_name[64];
    char client_model[32];
    char dacp_id[24];         /* the sender's remote-control service, see note_remote_control() */
    char active_remote[24];
    _Atomic bool stream_lost;
};

typedef struct {
    char key[64];
    uint8_t *data;
    size_t len;
    uint64_t used_ns;
} photo_t;

struct airplay_server {
    airplay_config_t cfg;
    airplay_events_t ev;
    const media_sink_ops_t *media;
    void *media_ctx;
    pairing_identity_t identity;
    uint64_t features;
    char pk_hex[65];
    char device_id_str[18];

    int listen_fd[2];
    uint16_t port;
    wakeup_t wake;
    pthread_t thread;
    bool thread_started;
    _Atomic bool running;
    _Atomic bool disconnect_requested;
    _Atomic bool session_active;

    conn_t *conns[MAX_CONNECTIONS];
    session_t *active;
    uint32_t next_session_id;
    uint64_t last_media_ns;
    uint64_t last_media_count;

    char pin[8];
    bool pin_active;
    uint64_t pin_since_ns;
    conn_t *pin_owner;
    float volume_db;

    pthread_mutex_t paired_lock;
    char **paired;
    int paired_count;

    photo_t photos[PHOTO_CACHE_SLOTS];
    size_t photo_bytes;
    bool photo_shown;
};

/* ------------------------------------------------------------------------- */
/* identity, features, TXT records                                            */

uint64_t airplay_features(const airplay_config_t *config) {
    uint64_t f = FEATURES_BASE;
    if (config->require_pin) {
        f |= FEATURE_LEGACY_PAIRING;
    }
    if (config->hevc) {
        f |= FEATURE_SCREEN_MULTI_CODEC;
    }
    return f;
}

void airplay_public_key_hex(const airplay_server_t *s, char out[65]) {
    memcpy(out, s->pk_hex, 65);
}

void airplay_raop_name(const airplay_config_t *config, char *out, size_t cap) {
    char id[13];
    format_device_id_compact(config->device_id, id);
    snprintf(out, cap, "%s@%s", id, config->name);
}

static int txt_add(txt_entry_t *out, int max, int n, const char *key, const char *value) {
    if (n < max) {
        str_copy(out[n].key, sizeof(out[n].key), key);
        str_copy(out[n].value, sizeof(out[n].value), value);
        n++;
    }
    return n;
}

static void features_string(uint64_t features, char *out, size_t cap) {
    snprintf(out, cap, "0x%X,0x%X", (unsigned) (features & 0xffffffffu), (unsigned) (features >> 32));
}

int airplay_txt_airplay(const airplay_server_t *s, txt_entry_t *out, int max) {
    char features[32];
    features_string(s->features, features, sizeof(features));
    int n = 0;
    n = txt_add(out, max, n, "deviceid", s->device_id_str);
    n = txt_add(out, max, n, "features", features);
    n = txt_add(out, max, n, "flags", "0x4");
    n = txt_add(out, max, n, "model", AIRPLAY_MODEL);
    n = txt_add(out, max, n, "pk", s->pk_hex);
    n = txt_add(out, max, n, "pi", s->cfg.public_id);
    n = txt_add(out, max, n, "pw", s->cfg.require_pin ? "true" : "false");
    n = txt_add(out, max, n, "srcvers", AIRPLAY_SOURCE_VERSION);
    n = txt_add(out, max, n, "vv", "2");
    return n;
}

int airplay_txt_raop(const airplay_server_t *s, txt_entry_t *out, int max) {
    char features[32];
    features_string(s->features, features, sizeof(features));
    int n = 0;
    n = txt_add(out, max, n, "ch", "2");
    n = txt_add(out, max, n, "cn", "0,1,2,3");
    n = txt_add(out, max, n, "da", "true");
    n = txt_add(out, max, n, "et", "0,3,5");
    n = txt_add(out, max, n, "vv", "2");
    n = txt_add(out, max, n, "ft", features);
    n = txt_add(out, max, n, "am", AIRPLAY_MODEL);
    n = txt_add(out, max, n, "md", "0,1,2");
    n = txt_add(out, max, n, "rhd", "5.6.0.0");
    n = txt_add(out, max, n, "pw", s->cfg.require_pin ? "true" : "false");
    n = txt_add(out, max, n, "sr", "44100");
    n = txt_add(out, max, n, "ss", "16");
    n = txt_add(out, max, n, "sv", "false");
    n = txt_add(out, max, n, "tp", "UDP");
    n = txt_add(out, max, n, "txtvers", "1");
    n = txt_add(out, max, n, "sf", s->cfg.require_pin ? "0x8c" : "0x4");
    n = txt_add(out, max, n, "vs", AIRPLAY_SOURCE_VERSION);
    n = txt_add(out, max, n, "vn", "65537");
    n = txt_add(out, max, n, "pk", s->pk_hex);
    return n;
}

size_t airplay_txt_encode(const txt_entry_t *entries, int count, uint8_t *out, size_t cap) {
    size_t pos = 0;
    for (int i = 0; i < count; i++) {
        size_t klen = strlen(entries[i].key);
        size_t vlen = strlen(entries[i].value);
        size_t len = klen + 1 + vlen;
        if (len > 255 || pos + 1 + len > cap) {
            return 0;
        }
        out[pos++] = (uint8_t) len;
        memcpy(out + pos, entries[i].key, klen);
        pos += klen;
        out[pos++] = '=';
        memcpy(out + pos, entries[i].value, vlen);
        pos += vlen;
    }
    return pos;
}

/* ------------------------------------------------------------------------- */
/* paired clients                                                             */

void airplay_server_set_paired_clients(airplay_server_t *s, const char *const *keys, int count) {
    pthread_mutex_lock(&s->paired_lock);
    for (int i = 0; i < s->paired_count; i++) {
        free(s->paired[i]);
    }
    free(s->paired);
    s->paired = NULL;
    s->paired_count = 0;
    if (count > 0) {
        s->paired = (char **) calloc((size_t) count, sizeof(char *));
        if (s->paired) {
            for (int i = 0; i < count; i++) {
                if (keys[i] && (s->paired[s->paired_count] = strdup(keys[i])) != NULL) {
                    s->paired_count++;
                }
            }
        }
    }
    pthread_mutex_unlock(&s->paired_lock);
}

static bool is_paired(airplay_server_t *s, const char *key_b64) {
    bool found = false;
    pthread_mutex_lock(&s->paired_lock);
    for (int i = 0; i < s->paired_count && !found; i++) {
        found = strcmp(s->paired[i], key_b64) == 0;
    }
    pthread_mutex_unlock(&s->paired_lock);
    return found;
}

static void add_paired(airplay_server_t *s, const char *key_b64) {
    pthread_mutex_lock(&s->paired_lock);
    bool found = false;
    for (int i = 0; i < s->paired_count && !found; i++) {
        found = strcmp(s->paired[i], key_b64) == 0;
    }
    if (!found) {
        char **grown = (char **) realloc(s->paired, ((size_t) s->paired_count + 1) * sizeof(char *));
        if (grown) {
            s->paired = grown;
            s->paired[s->paired_count] = strdup(key_b64);
            if (s->paired[s->paired_count]) {
                s->paired_count++;
            }
        }
    }
    pthread_mutex_unlock(&s->paired_lock);
}

static void key_to_b64(const uint8_t key[ED25519_KEY_SIZE], char out[48]) {
    base64_encode(key, ED25519_KEY_SIZE, out);
}

/* ------------------------------------------------------------------------- */
/* sessions                                                                   */

static void on_stream_lost(void *ctx) {
    session_t *session = (session_t *) ctx;
    atomic_store(&session->stream_lost, true);
    wakeup_signal(&session->server->wake);
}

static void session_stop_video(session_t *session) {
    airplay_server_t *s = session->server;
    if (session->mirror) {
        mirror_stop(session->mirror);
        session->mirror = NULL;
        if (s->media->video_stop) {
            s->media->video_stop(s->media_ctx);
        }
    }
}

static void session_stop_audio(session_t *session) {
    airplay_server_t *s = session->server;
    if (session->audio) {
        audio_stop(session->audio);
        session->audio = NULL;
        if (s->media->audio_stop) {
            s->media->audio_stop(s->media_ctx);
        }
    }
}

static void session_destroy(airplay_server_t *s, session_t *session) {
    if (!session) {
        return;
    }
    session_stop_video(session);
    session_stop_audio(session);
    ntp_client_stop(session->ntp);
    session->ntp = NULL;
    if (session->owner) {
        session->owner->session = NULL;
    }
    if (s->active == session) {
        s->active = NULL;
        atomic_store(&s->session_active, false);
        LOG_I(SESSION, "session %u ended", session->id);
        if (s->ev.session_ended) {
            s->ev.session_ended(s->ev.ctx);
        }
    }
    secure_zero(session->key, sizeof(session->key));
    free(session);
}

/* ------------------------------------------------------------------------- */
/* photo cache                                                                */

static photo_t *photo_cache_find(airplay_server_t *s, const char *key) {
    for (int i = 0; i < PHOTO_CACHE_SLOTS; i++) {
        if (s->photos[i].data && strcmp(s->photos[i].key, key) == 0) {
            return &s->photos[i];
        }
    }
    return NULL;
}

static void photo_cache_drop(airplay_server_t *s, photo_t *p) {
    s->photo_bytes -= p->len;
    free(p->data);
    memset(p, 0, sizeof(*p));
}

static void photo_cache_clear(airplay_server_t *s) {
    for (int i = 0; i < PHOTO_CACHE_SLOTS; i++) {
        if (s->photos[i].data) {
            photo_cache_drop(s, &s->photos[i]);
        }
    }
}

/* Keeps a photo under its key, evicting the least recently used ones to stay within the
 * limits. Takes ownership of data and returns true, or returns false if it cannot be kept. */
static bool photo_cache_adopt(airplay_server_t *s, const char *key, uint8_t *data, size_t len) {
    if (!key[0] || strlen(key) >= sizeof(s->photos[0].key) || len == 0 || len > PHOTO_CACHE_BYTES) {
        return false;
    }
    photo_t *same = photo_cache_find(s, key);
    if (same) {
        photo_cache_drop(s, same);
    }
    for (;;) {
        photo_t *slot = NULL;
        photo_t *oldest = NULL;
        for (int i = 0; i < PHOTO_CACHE_SLOTS; i++) {
            photo_t *p = &s->photos[i];
            if (!p->data) {
                slot = slot ? slot : p;
            } else if (!oldest || p->used_ns < oldest->used_ns) {
                oldest = p;
            }
        }
        if (slot && s->photo_bytes + len <= PHOTO_CACHE_BYTES) {
            str_copy(slot->key, sizeof(slot->key), key);
            slot->data = data;
            slot->len = len;
            slot->used_ns = time_mono_ns();
            s->photo_bytes += len;
            return true;
        }
        if (!oldest) {
            return false;
        }
        photo_cache_drop(s, oldest);
    }
}

static void photo_show(airplay_server_t *s, const uint8_t *data, size_t len) {
    s->photo_shown = true;
    if (s->ev.photo) {
        s->ev.photo(s->ev.ctx, data, len);
    }
}

static void photo_session_end(airplay_server_t *s) {
    photo_cache_clear(s);
    if (s->photo_shown) {
        s->photo_shown = false;
        LOG_I(AIRPLAY, "photo session ended");
        if (s->ev.photo_stop) {
            s->ev.photo_stop(s->ev.ctx);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* connections                                                                */

static void hide_pin(airplay_server_t *s);

static void conn_close(airplay_server_t *s, int index) {
    conn_t *c = s->conns[index];
    if (!c) {
        return;
    }
    if (s->pin_owner == c) {
        /* the sender that asked for a PIN went away: take the code off the screen */
        s->pin_owner = NULL;
        hide_pin(s);
    }
    if (c->session) {
        session_destroy(s, c->session);
    }
    if (c->event_channel) {
        /* the Photos app closes its event connection when the user leaves AirPlay */
        photo_session_end(s);
    }
    net_close(&c->fd);
    pairing_session_clear(&c->pair);
    fairplay_reset(&c->fp);
    free(c->rx);
    free(c);
    s->conns[index] = NULL;
}

static int conn_index(airplay_server_t *s, const conn_t *c) {
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (s->conns[i] == c) {
            return i;
        }
    }
    return -1;
}

static void configure_control_socket(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#ifdef TCP_KEEPIDLE
    int idle = 15;
    int interval = 5;
    int count = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
#endif
}

static void accept_connections(airplay_server_t *s, int listen_fd) {
    for (;;) {
        struct sockaddr_storage peer;
        socklen_t peer_len = sizeof(peer);
        int fd = accept4(listen_fd, (struct sockaddr *) &peer, &peer_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            return;
        }
        net_normalize_addr(&peer, NULL);
        char addr[64];
        net_addr_to_string(&peer, addr, sizeof(addr));
        if (!net_is_lan_peer(&peer)) {
            LOG_W(NETWORK, "refused connection from outside the local network (%s)", addr);
            stat_add(&g_stats.connections_rejected, 1);
            close(fd);
            continue;
        }
        int slot = -1;
        for (int i = 0; i < MAX_CONNECTIONS; i++) {
            if (!s->conns[i]) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            /* make room by dropping the oldest connection that has no session */
            uint64_t oldest = UINT64_MAX;
            for (int i = 0; i < MAX_CONNECTIONS; i++) {
                if (s->conns[i] && !s->conns[i]->session && s->conns[i]->created_ns < oldest) {
                    oldest = s->conns[i]->created_ns;
                    slot = i;
                }
            }
            if (slot < 0) {
                LOG_W(NETWORK, "too many connections, refusing %s", addr);
                close(fd);
                continue;
            }
            conn_close(s, slot);
        }
        conn_t *c = (conn_t *) calloc(1, sizeof(conn_t));
        if (!c) {
            close(fd);
            continue;
        }
        c->fd = fd;
        c->peer = peer;
        c->created_ns = c->last_rx_ns = time_mono_ns();
        pairing_session_init(&c->pair);
        fairplay_reset(&c->fp);
        configure_control_socket(fd);
        s->conns[slot] = c;
        LOG_I(NETWORK, "connection from %s", addr);
    }
}

/* ------------------------------------------------------------------------- */
/* request helpers                                                            */

static bp_node_t *parse_plist_body(const rtsp_request_t *req) {
    if (!req->body || req->body_len == 0) {
        return NULL;
    }
    return bp_parse(req->body, req->body_len);
}

static void reply_plist(rtsp_reply_t *reply, bp_node_t *root) {
    uint8_t *data = NULL;
    size_t len = 0;
    if (root && bp_write(root, &data, &len) == 0) {
        rtsp_reply_body(reply, CT_BPLIST, data, len);
    } else {
        rtsp_reply_status(reply, 500, "Internal Server Error");
    }
    bp_free(root);
}

static void dict_set_uint(bp_node_t *d, const char *key, uint64_t v) {
    bp_dict_set(d, key, bp_new_uint(v));
}

/* ------------------------------------------------------------------------- */
/* handlers                                                                   */

static void handle_info(airplay_server_t *s, conn_t *c, const rtsp_request_t *req, rtsp_reply_t *reply) {
    (void) c;
    const char *qualifier = NULL;
    bp_node_t *request = parse_plist_body(req);
    if (request) {
        const char *q = bp_get_string(bp_array_get(bp_dict_get(request, "qualifier"), 0));
        if (q && (strcmp(q, "txtAirPlay") == 0 || strcmp(q, "txtRAOP") == 0)) {
            qualifier = strcmp(q, "txtAirPlay") == 0 ? "txtAirPlay" : "txtRAOP";
        }
    } else if (!rtsp_header(req, "CSeq")) {
        /* discovery without mDNS: GET /info?txtAirPlay&txtRAOP */
        if (strstr(req->url, "txtAirPlay")) {
            qualifier = "txtAirPlay";
        } else if (strstr(req->url, "txtRAOP")) {
            qualifier = "txtRAOP";
        }
    }

    bp_node_t *root = bp_new_dict();
    if (qualifier) {
        txt_entry_t entries[24];
        uint8_t txt[1024];
        int n = strcmp(qualifier, "txtAirPlay") == 0 ? airplay_txt_airplay(s, entries, 24)
                                                     : airplay_txt_raop(s, entries, 24);
        size_t len = airplay_txt_encode(entries, n, txt, sizeof(txt));
        bp_dict_set(root, qualifier, bp_new_data(txt, len));
        bp_free(request);
        reply_plist(reply, root);
        return;
    }
    bp_free(request);

    uint8_t pk[ED25519_KEY_SIZE];
    memcpy(pk, s->identity.public_key, sizeof(pk));
    bp_dict_set(root, "deviceID", bp_new_string(s->device_id_str));
    bp_dict_set(root, "macAddress", bp_new_string(s->device_id_str));
    bp_dict_set(root, "pk", bp_new_data(pk, sizeof(pk)));
    dict_set_uint(root, "features", s->features);
    bp_dict_set(root, "name", bp_new_string(s->cfg.name));
    bp_dict_set(root, "pi", bp_new_string(s->cfg.public_id));
    dict_set_uint(root, "vv", 2);
    dict_set_uint(root, "statusFlags", 68);
    dict_set_uint(root, "keepAliveLowPower", 1);
    bp_dict_set(root, "sourceVersion", bp_new_string(AIRPLAY_SOURCE_VERSION));
    bp_dict_set(root, "keepAliveSendStatsAsBody", bp_new_bool(true));
    bp_dict_set(root, "model", bp_new_string(AIRPLAY_MODEL));
    bp_dict_set(root, "initialVolume", bp_new_real(s->volume_db));

    bp_node_t *latencies = bp_new_array();
    bp_node_t *formats = bp_new_array();
    for (int type = 100; type <= 101; type++) {
        bp_node_t *l = bp_new_dict();
        dict_set_uint(l, "type", (uint64_t) type);
        bp_dict_set(l, "audioType", bp_new_string("default"));
        dict_set_uint(l, "inputLatencyMicros", 0);
        bp_dict_set(l, "outputLatencyMicros", bp_new_bool(false));
        bp_array_append(latencies, l);

        bp_node_t *f = bp_new_dict();
        dict_set_uint(f, "type", (uint64_t) type);
        dict_set_uint(f, "audioInputFormats", 0x3fffffc);
        dict_set_uint(f, "audioOutputFormats", 0x3fffffc);
        bp_array_append(formats, f);
    }
    bp_dict_set(root, "audioLatencies", latencies);
    bp_dict_set(root, "audioFormats", formats);

    char uuid[40];
    snprintf(uuid, sizeof(uuid), "e0ff8a27-6738-3d56-8a16-%02x%02x%02x%02x%02x%02x",
             s->cfg.device_id[0], s->cfg.device_id[1], s->cfg.device_id[2],
             s->cfg.device_id[3], s->cfg.device_id[4], s->cfg.device_id[5]);
    bp_node_t *displays = bp_new_array();
    bp_node_t *d = bp_new_dict();
    bp_dict_set(d, "uuid", bp_new_string(uuid));
    dict_set_uint(d, "widthPhysical", 0);
    dict_set_uint(d, "heightPhysical", 0);
    dict_set_uint(d, "width", (uint64_t) s->cfg.display_width);
    dict_set_uint(d, "height", (uint64_t) s->cfg.display_height);
    dict_set_uint(d, "widthPixels", (uint64_t) s->cfg.display_width);
    dict_set_uint(d, "heightPixels", (uint64_t) s->cfg.display_height);
    bp_dict_set(d, "rotation", bp_new_bool(false));
    bp_dict_set(d, "refreshRate", bp_new_real(1.0 / (double) (s->cfg.display_fps > 0 ? s->cfg.display_fps : 60)));
    dict_set_uint(d, "maxFPS", (uint64_t) s->cfg.display_fps);
    bp_dict_set(d, "overscanned", bp_new_bool(false));
    dict_set_uint(d, "features", 14);
    bp_array_append(displays, d);
    bp_dict_set(root, "displays", displays);

    reply_plist(reply, root);
}

static void handle_pair_setup(airplay_server_t *s, conn_t *c, const rtsp_request_t *req, rtsp_reply_t *reply) {
    if (req->body_len != ED25519_KEY_SIZE) {
        rtsp_reply_status(reply, 400, "Bad Request");
        return;
    }
    if (c->pair.state == PAIR_STATE_INITIAL) {
        c->pair.state = PAIR_STATE_SETUP;
    }
    rtsp_reply_body_copy(reply, CT_OCTET, s->identity.public_key, ED25519_KEY_SIZE);
    LOG_D(PAIRING, "pair-setup");
}

static void handle_pair_verify(airplay_server_t *s, conn_t *c, const rtsp_request_t *req, rtsp_reply_t *reply) {
    const uint8_t *d = req->body;
    if (!d || req->body_len < 4) {
        rtsp_reply_status(reply, 400, "Bad Request");
        return;
    }
    if (d[0] == 1) {
        if (req->body_len != 4 + X25519_KEY_SIZE + ED25519_KEY_SIZE) {
            rtsp_reply_status(reply, 400, "Bad Request");
            return;
        }
        const uint8_t *client_ecdh = d + 4;
        const uint8_t *client_ed = d + 4 + X25519_KEY_SIZE;
        if (s->cfg.require_pin) {
            char b64[48];
            key_to_b64(client_ed, b64);
            bool trusted = (c->pair.pin_paired &&
                            crypto_memcmp(c->pair.pin_client_public, client_ed, ED25519_KEY_SIZE) == 0) ||
                           is_paired(s, b64);
            if (!trusted) {
                /* An empty answer makes the sender fall back to PIN pairing. */
                LOG_I(PAIRING, "sender is not paired yet, PIN required");
                return;
            }
            c->pair_trusted = true;
        }
        uint8_t out[PAIR_VERIFY_REPLY_LEN];
        if (pairing_verify_start(&c->pair, &s->identity, client_ecdh, client_ed, out) != 0) {
            LOG_W(PAIRING, "pair-verify could not be started");
            rtsp_reply_status(reply, 470, "Connection Authorization Required");
            reply->close_connection = true;
            return;
        }
        rtsp_reply_body_copy(reply, CT_OCTET, out, sizeof(out));
    } else if (d[0] == 0) {
        if (req->body_len != 4 + ED25519_SIG_SIZE) {
            rtsp_reply_status(reply, 400, "Bad Request");
            return;
        }
        if (pairing_verify_finish(&c->pair, d + 4) != 0) {
            LOG_W(PAIRING, "pair-verify signature rejected");
            rtsp_reply_status(reply, 470, "Connection Authorization Required");
            reply->close_connection = true;
            return;
        }
        LOG_I(PAIRING, "pair-verify completed");
        reply->content_type = CT_OCTET;
    } else {
        rtsp_reply_status(reply, 400, "Bad Request");
    }
}

static void handle_fp_setup(airplay_server_t *s, conn_t *c, const rtsp_request_t *req, rtsp_reply_t *reply) {
    (void) s;
    if (req->body_len == FAIRPLAY_SETUP_REQUEST_LEN) {
        uint8_t out[FAIRPLAY_SETUP_REPLY_LEN];
        if (fairplay_setup(&c->fp, req->body, req->body_len, out) != 0) {
            LOG_W(AIRPLAY, "unsupported FairPlay setup request");
            rtsp_reply_status(reply, 501, "Not Implemented");
            return;
        }
        rtsp_reply_body_copy(reply, CT_OCTET, out, sizeof(out));
    } else if (req->body_len == FAIRPLAY_HANDSHAKE_REQUEST_LEN) {
        uint8_t out[FAIRPLAY_HANDSHAKE_REPLY_LEN];
        if (fairplay_handshake(&c->fp, req->body, req->body_len, out) != 0) {
            LOG_W(AIRPLAY, "unsupported FairPlay handshake");
            rtsp_reply_status(reply, 501, "Not Implemented");
            return;
        }
        rtsp_reply_body_copy(reply, CT_OCTET, out, sizeof(out));
    } else {
        rtsp_reply_status(reply, 400, "Bad Request");
    }
}

static void hide_pin(airplay_server_t *s) {
    if (s->pin_active) {
        s->pin_active = false;
        secure_zero(s->pin, sizeof(s->pin));
        if (s->ev.pin_display) {
            s->ev.pin_display(s->ev.ctx, NULL);
        }
    }
}

static void handle_pair_pin_start(airplay_server_t *s, conn_t *c, const rtsp_request_t *req, rtsp_reply_t *reply) {
    (void) req;
    if (!s->cfg.require_pin) {
        rtsp_reply_status(reply, 404, "Not Found");
        return;
    }
    uint16_t r = 0;
    crypto_random(&r, sizeof(r));
    snprintf(s->pin, sizeof(s->pin), "%04u", (unsigned) (r % 10000));
    s->pin_active = true;
    s->pin_since_ns = time_mono_ns();
    s->pin_owner = c;
    LOG_I(PAIRING, "showing pairing PIN");
    if (s->ev.pin_display) {
        s->ev.pin_display(s->ev.ctx, s->pin);
    }
}

static void pin_failed(rtsp_reply_t *reply) {
    rtsp_reply_status(reply, 470, "Client Authentication Failure");
}

static void handle_pair_setup_pin(airplay_server_t *s, conn_t *c, const rtsp_request_t *req, rtsp_reply_t *reply) {
    if (!s->cfg.require_pin || !s->pin_active) {
        pin_failed(reply);
        return;
    }
    bp_node_t *root = parse_plist_body(req);
    if (!root) {
        pin_failed(reply);
        return;
    }
    const char *method = bp_get_string(bp_dict_get(root, "method"));
    const char *user = bp_get_string(bp_dict_get(root, "user"));
    size_t pk_len = 0;
    size_t proof_len = 0;
    size_t epk_len = 0;
    size_t tag_len = 0;
    const uint8_t *pk = bp_get_data(bp_dict_get(root, "pk"), &pk_len);
    const uint8_t *proof = bp_get_data(bp_dict_get(root, "proof"), &proof_len);
    const uint8_t *epk = bp_get_data(bp_dict_get(root, "epk"), &epk_len);
    const uint8_t *tag = bp_get_data(bp_dict_get(root, "authTag"), &tag_len);

    if (method && user) {
        /* step 1: start SRP */
        uint8_t salt[SRP_SALT_LEN];
        uint8_t B[SRP_MODULUS_LEN];
        size_t B_len = 0;
        if (strcmp(method, "pin") != 0 || strlen(user) > 32 ||
            pairing_pin_start(&c->pair, user, s->pin, salt, B, &B_len) != 0) {
            pin_failed(reply);
        } else {
            bp_node_t *out = bp_new_dict();
            bp_dict_set(out, "pk", bp_new_data(B, B_len));
            bp_dict_set(out, "salt", bp_new_data(salt, sizeof(salt)));
            reply_plist(reply, out);
        }
    } else if (pk && proof) {
        /* step 2: verify the client proof */
        uint8_t M2[SRP_PROOF_LEN];
        if (pairing_pin_verify(&c->pair, pk, pk_len, proof, proof_len, M2) != 0) {
            LOG_W(PAIRING, "wrong PIN entered");
            pin_failed(reply);
        } else {
            bp_node_t *out = bp_new_dict();
            bp_dict_set(out, "proof", bp_new_data(M2, sizeof(M2)));
            reply_plist(reply, out);
        }
    } else if (epk && tag && epk_len == ED25519_KEY_SIZE && tag_len == 16) {
        /* step 3: exchange long-term keys */
        uint8_t out_epk[ED25519_KEY_SIZE];
        uint8_t out_tag[16];
        if (pairing_pin_exchange(&c->pair, &s->identity, epk, tag, out_epk, out_tag) != 0) {
            LOG_W(PAIRING, "PIN pairing key exchange failed");
            pin_failed(reply);
        } else {
            LOG_I(PAIRING, "PIN pairing completed");
            hide_pin(s);
            bp_node_t *out = bp_new_dict();
            bp_dict_set(out, "epk", bp_new_data(out_epk, sizeof(out_epk)));
            bp_dict_set(out, "authTag", bp_new_data(out_tag, sizeof(out_tag)));
            reply_plist(reply, out);
        }
    } else {
        pin_failed(reply);
    }
    bp_free(root);
}

static bool old_protocol_client(const rtsp_request_t *req) {
    const char *ua = rtsp_header(req, "User-Agent");
    return ua && strstr(ua, "AirMyPC") != NULL;
}

static bool setup_session(airplay_server_t *s, conn_t *c, const rtsp_request_t *req, bp_node_t *root,
                          bp_node_t *out) {
    size_t ekey_len = 0;
    size_t eiv_len = 0;
    const uint8_t *ekey = bp_get_data(bp_dict_get(root, "ekey"), &ekey_len);
    const uint8_t *eiv = bp_get_data(bp_dict_get(root, "eiv"), &eiv_len);
    if (!ekey || !eiv || ekey_len != FAIRPLAY_EKEY_LEN || eiv_len < 16) {
        LOG_W(AIRPLAY, "SETUP without a valid stream key");
        return false;
    }
    if (s->cfg.require_pin && (!c->pair_trusted || c->pair.state != PAIR_STATE_VERIFIED)) {
        LOG_W(PAIRING, "refusing SETUP from a sender that is not paired");
        return false;
    }

    uint8_t key[16];
    if (fairplay_decrypt(&c->fp, ekey, ekey_len, key) != 0) {
        LOG_W(AIRPLAY, "SETUP before the FairPlay handshake");
        return false;
    }
    uint8_t shared[X25519_KEY_SIZE];
    if (!old_protocol_client(req) && pairing_shared_secret(&c->pair, shared)) {
        uint8_t hash[SHA512_SIZE];
        sha512_2(key, sizeof(key), shared, sizeof(shared), hash);
        memcpy(key, hash, 16);
        secure_zero(hash, sizeof(hash));
        secure_zero(shared, sizeof(shared));
    }

    /* One sender at a time: a new sender replaces the current one. */
    if (s->active && s->active->owner != c) {
        conn_t *old = s->active->owner;
        LOG_I(SESSION, "new sender replaces the current session");
        session_destroy(s, s->active);
        if (old) {
            old->closing = true;
        }
    }
    if (c->session) {
        session_destroy(s, c->session);
    }

    session_t *session = (session_t *) calloc(1, sizeof(session_t));
    if (!session) {
        return false;
    }
    session->server = s;
    session->owner = c;
    session->id = ++s->next_session_id;
    memcpy(session->key, key, sizeof(key));
    memcpy(session->iv, eiv, 16);
    secure_zero(key, sizeof(key));
    const char *name = bp_get_string(bp_dict_get(root, "name"));
    const char *model = bp_get_string(bp_dict_get(root, "model"));
    if (!name || !name[0]) {
        name = "iPhone";
    }
    sanitize_utf8(session->client_name, sizeof(session->client_name), name, strlen(name));
    sanitize_utf8(session->client_model, sizeof(session->client_model), model ? model : "", model ? strlen(model) : 0);

    uint64_t timing_port = 0;
    bp_get_uint(bp_dict_get(root, "timingPort"), &timing_port);
    uint16_t local_timing = 0;
    session->ntp = ntp_client_start(&c->peer, timing_port <= 0xffff ? (uint16_t) timing_port : 0, &local_timing);

    c->session = session;
    s->active = session;
    atomic_store(&s->session_active, true);
    s->last_media_ns = time_mono_ns();
    stat_add(&g_stats.sessions_started, 1);
    stats_reset_session();

    if (s->cfg.require_pin && c->pair.pin_paired) {
        /* remember the sender so the PIN is only needed once */
        char b64[48];
        key_to_b64(c->pair.pin_client_public, b64);
        add_paired(s, b64);
        if (s->ev.client_paired) {
            s->ev.client_paired(s->ev.ctx, b64, session->client_name);
        }
    }

    LOG_I(SESSION, "session %u started (%s)", session->id, session->client_model[0] ? session->client_model : "unknown model");
    if (s->ev.session_started) {
        s->ev.session_started(s->ev.ctx, session->client_name, session->client_model);
    }

    dict_set_uint(out, "timingPort", local_timing);
    dict_set_uint(out, "eventPort", 0);
    return true;
}

static bp_node_t *setup_mirror_stream(airplay_server_t *s, session_t *session, bp_node_t *stream) {
    uint64_t connection_id = 0;
    if (!bp_get_uint(bp_dict_get(stream, "streamConnectionID"), &connection_id)) {
        int64_t signed_id = 0;
        if (bp_get_int(bp_dict_get(stream, "streamConnectionID"), &signed_id)) {
            connection_id = (uint64_t) signed_id;
        }
    }
    session_stop_video(session);
    mirror_params_t p;
    memset(&p, 0, sizeof(p));
    p.ops = s->media;
    p.ops_ctx = s->media_ctx;
    memcpy(p.session_key, session->key, 16);
    p.stream_connection_id = connection_id;
    p.peer = session->owner->peer;
    p.on_closed = on_stream_lost;
    p.closed_ctx = session;
    if (s->media->video_start) {
        s->media->video_start(s->media_ctx);
    }
    uint16_t port = 0;
    session->mirror = mirror_start(&p, &port);
    secure_zero(p.session_key, sizeof(p.session_key));
    if (!session->mirror) {
        if (s->media->video_stop) {
            s->media->video_stop(s->media_ctx);
        }
        return NULL;
    }
    LOG_I(VIDEO, "mirroring stream set up");
    bp_node_t *out = bp_new_dict();
    dict_set_uint(out, "dataPort", port);
    dict_set_uint(out, "type", 110);
    return out;
}

static bp_node_t *setup_audio_stream(airplay_server_t *s, session_t *session, bp_node_t *stream) {
    uint64_t ct = 0;
    uint64_t spf = 0;
    uint64_t control_port = 0;
    bool using_screen = false;
    bool is_media = false;
    bp_get_uint(bp_dict_get(stream, "ct"), &ct);
    bp_get_uint(bp_dict_get(stream, "spf"), &spf);
    bp_get_uint(bp_dict_get(stream, "controlPort"), &control_port);
    bp_get_bool(bp_dict_get(stream, "usingScreen"), &using_screen);
    bp_get_bool(bp_dict_get(stream, "isMedia"), &is_media);

    if (ct != AUDIO_CT_ALAC && ct != AUDIO_CT_AAC_LC && ct != AUDIO_CT_AAC_ELD) {
        LOG_W(AUDIO, "unsupported audio compression type %llu", (unsigned long long) ct);
        return NULL;
    }
    session_stop_audio(session);

    audio_params_t p;
    memset(&p, 0, sizeof(p));
    p.ops = s->media;
    p.ops_ctx = s->media_ctx;
    memcpy(p.key, session->key, 16);
    memcpy(p.iv, session->iv, 16);
    p.peer = session->owner->peer;
    p.peer_control_port = control_port <= 0xffff ? (uint16_t) control_port : 0;
    p.format.ct = (int) ct;
    p.format.samples_per_frame = (spf > 0 && spf <= 4096) ? (int) spf
                                 : (ct == AUDIO_CT_ALAC ? 352 : (ct == AUDIO_CT_AAC_ELD ? 480 : 1024));
    p.format.sample_rate = 44100;
    p.format.channels = 2;
    p.format.using_screen = using_screen;
    p.format.is_media = is_media;

    if (s->media->audio_start && !s->media->audio_start(s->media_ctx, &p.format)) {
        LOG_W(AUDIO, "audio output could not be started");
    }
    if (s->media->audio_volume) {
        s->media->audio_volume(s->media_ctx, s->volume_db);
    }
    uint16_t dport = 0;
    uint16_t cport = 0;
    session->audio = audio_start(&p, &dport, &cport);
    secure_zero(p.key, sizeof(p.key));
    if (!session->audio) {
        if (s->media->audio_stop) {
            s->media->audio_stop(s->media_ctx);
        }
        return NULL;
    }
    atomic_store_explicit(&g_stats.audio_ct, (int32_t) ct, memory_order_relaxed);
    LOG_I(AUDIO, "audio stream set up (ct=%llu, spf=%d)", (unsigned long long) ct, p.format.samples_per_frame);
    bp_node_t *out = bp_new_dict();
    dict_set_uint(out, "dataPort", dport);
    dict_set_uint(out, "controlPort", cport);
    dict_set_uint(out, "type", 96);
    return out;
}

static void handle_setup(airplay_server_t *s, conn_t *c, const rtsp_request_t *req, rtsp_reply_t *reply) {
    bp_node_t *root = parse_plist_body(req);
    if (!root || bp_type(root) != BP_DICT) {
        bp_free(root);
        rtsp_reply_status(reply, 400, "Bad Request");
        return;
    }
    bp_node_t *out = bp_new_dict();
    if (bp_dict_get(root, "ekey")) {
        if (!setup_session(s, c, req, root, out)) {
            bp_free(out);
            bp_free(root);
            rtsp_reply_status(reply, 403, "Forbidden");
            reply->close_connection = true;
            return;
        }
    }

    bp_node_t *streams = bp_dict_get(root, "streams");
    if (streams) {
        session_t *session = c->session;
        if (!session || bp_type(streams) != BP_ARRAY) {
            bp_free(out);
            bp_free(root);
            rtsp_reply_status(reply, 455, "Method Not Valid In This State");
            return;
        }
        bp_node_t *result = bp_new_array();
        for (size_t i = 0; i < bp_count(streams) && i < 4; i++) {
            bp_node_t *stream = bp_array_get(streams, i);
            uint64_t type = 0;
            bp_get_uint(bp_dict_get(stream, "type"), &type);
            bp_node_t *desc = NULL;
            if (type == 110) {
                desc = setup_mirror_stream(s, session, stream);
            } else if (type == 96) {
                desc = setup_audio_stream(s, session, stream);
            } else {
                LOG_W(AIRPLAY, "sender requested unsupported stream type %llu", (unsigned long long) type);
                reply->close_connection = true;
            }
            if (desc) {
                bp_array_append(result, desc);
            }
        }
        bp_dict_set(out, "streams", result);
    }
    bp_free(root);
    reply_plist(reply, out);
}

static void handle_record(airplay_server_t *s, conn_t *c, const rtsp_request_t *req, rtsp_reply_t *reply) {
    (void) s;
    (void) c;
    (void) req;
    /* 0.25 s in samples at 44.1 kHz, as announced by UxPlay */
    rtsp_reply_header(reply, "Audio-Latency", "11025");
    rtsp_reply_header(reply, "Audio-Jack-Status", "connected; type=analog");
}

static void handle_get_parameter(airplay_server_t *s, conn_t *c, const rtsp_request_t *req, rtsp_reply_t *reply) {
    (void) c;
    const char *ct = rtsp_header(req, "Content-Type");
    if (!ct) {
        rtsp_reply_status(reply, 451, "Parameter Not Understood");
        return;
    }
    if (str_ieq(ct, CT_PARAMS) && req->body_len >= 6 && memcmp(req->body, "volume", 6) == 0) {
        char text[32];
        int n = snprintf(text, sizeof(text), "volume: %.6f\r\n", (double) s->volume_db);
        rtsp_reply_body_copy(reply, CT_PARAMS, text, (size_t) n);
    }
}

/* "start/current/end" in RTP timestamp units, each a 32-bit number. */
static bool parse_progress(const char *text, uint32_t out[3]) {
    const char *p = text;
    for (int i = 0; i < 3; i++) {
        size_t digits = strspn(p, "0123456789");
        long long v = parse_uint(p, digits, 0xffffffffLL);
        if (v < 0) {
            return false;
        }
        out[i] = (uint32_t) v;
        p += digits;
        if (i < 2 && *p++ != '/') {
            return false;
        }
    }
    return *p == '\0' || *p == '\r' || *p == '\n';
}

static void handle_set_parameter(airplay_server_t *s, conn_t *c, const rtsp_request_t *req, rtsp_reply_t *reply) {
    const char *ct = rtsp_header(req, "Content-Type");
    if (!ct) {
        rtsp_reply_status(reply, 451, "Parameter Not Understood");
        return;
    }
    /* What is playing is only reported for the session on screen, not for one being replaced. */
    bool active = c->session && c->session == s->active;
    if (str_ieq(ct, CT_DMAP)) {
        if (active && s->ev.track_info) {
            dmap_track_t track;
            dmap_parse_track(req->body, req->body_len, &track);
            s->ev.track_info(s->ev.ctx, track.title, track.artist, track.album);
        }
        return;
    }
    if (strncmp(ct, "image/", 6) == 0) {
        if (active && s->ev.artwork) {
            /* senders announce a track without cover with an empty image ("image/none") */
            s->ev.artwork(s->ev.ctx, req->body_len ? req->body : NULL, req->body_len);
        }
        return;
    }
    if (!str_ieq(ct, CT_PARAMS) || !req->body) {
        return;
    }
    char text[128];
    size_t n = MIN(req->body_len, sizeof(text) - 1);
    memcpy(text, req->body, n);
    text[n] = '\0';
    if (strncmp(text, "volume: ", 8) == 0) {
        char *end = NULL;
        float db = strtof(text + 8, &end);
        if (end != text + 8 && db == db) {
            if (db < -144.0f) {
                db = -144.0f;
            } else if (db > 0.0f) {
                db = 0.0f;
            }
            s->volume_db = db;
            if (c->session && c->session->audio && s->media->audio_volume) {
                s->media->audio_volume(s->media_ctx, db);
            }
            LOG_D(AUDIO, "volume %.1f dB", (double) db);
        }
    } else if (strncmp(text, "progress: ", 10) == 0) {
        uint32_t p[3];
        if (active && s->ev.progress && parse_progress(text + 10, p)) {
            s->ev.progress(s->ev.ctx, p[0], p[1], p[2]);
        }
    }
}

static void handle_flush(airplay_server_t *s, conn_t *c, const rtsp_request_t *req, rtsp_reply_t *reply) {
    (void) s;
    (void) reply;
    int next_seq = -1;
    const char *info = rtsp_header(req, "RTP-Info");
    if (info && strncmp(info, "seq=", 4) == 0) {
        const char *digits = info + 4;
        size_t len = strspn(digits, "0123456789");
        long long v = parse_uint(digits, len, 0xffff);
        next_seq = (int) v;
    }
    if (c->session && c->session->audio) {
        audio_request_flush(c->session->audio, next_seq);
    }
}

static void handle_teardown(airplay_server_t *s, conn_t *c, const rtsp_request_t *req, rtsp_reply_t *reply) {
    bool audio = false;
    bool video = false;
    bp_node_t *root = parse_plist_body(req);
    bp_node_t *streams = bp_dict_get(root, "streams");
    for (size_t i = 0; i < bp_count(streams); i++) {
        uint64_t type = 0;
        bp_get_uint(bp_dict_get(bp_array_get(streams, i), "type"), &type);
        audio |= type == 96;
        video |= type == 110;
    }
    bp_free(root);
    session_t *session = c->session;
    if (!session) {
        return;
    }
    if (audio || video) {
        if (video) {
            LOG_I(VIDEO, "mirroring stopped by the sender");
            session_stop_video(session);
        }
        if (audio) {
            session_stop_audio(session);
        }
        return;
    }
    rtsp_reply_header(reply, "Connection", "close");
    session_destroy(s, session);
}

/* The sender names its remote-control service (_dacp._tcp "iTunes_Ctrl_<DACP-ID>") and the
 * token commands must carry (Active-Remote) in the headers of its requests. Both end up in a
 * service name and an HTTP header on the Android side, so only the expected forms pass:
 * a 64-bit hex number and a 32-bit decimal one. */
static void note_remote_control(airplay_server_t *s, session_t *session, const rtsp_request_t *req) {
    const char *id = rtsp_header(req, "DACP-ID");
    const char *token = rtsp_header(req, "Active-Remote");
    if (!id || !token) {
        return;
    }
    size_t id_len = strlen(id);
    size_t token_len = strlen(token);
    if (id_len == 0 || id_len > 16 || strspn(id, "0123456789abcdefABCDEF") != id_len ||
        token_len == 0 || token_len > 10 || strspn(token, "0123456789") != token_len) {
        return;
    }
    if (strcmp(id, session->dacp_id) == 0 && strcmp(token, session->active_remote) == 0) {
        return;
    }
    str_copy(session->dacp_id, sizeof(session->dacp_id), id);
    str_copy(session->active_remote, sizeof(session->active_remote), token);
    if (s->ev.remote_control) {
        s->ev.remote_control(s->ev.ctx, session->dacp_id, session->active_remote);
    }
}

/* ---- HTTP: photos from the Photos app ---- */

static bool is_jpeg_or_png(const uint8_t *d, size_t n) {
    static const uint8_t kPng[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    return d && ((n >= 3 && d[0] == 0xff && d[1] == 0xd8 && d[2] == 0xff) ||
                 (n >= sizeof(kPng) && memcmp(d, kPng, sizeof(kPng)) == 0));
}

static bool path_is(const char *url, const char *path) {
    size_t n = strlen(path);
    return strncmp(url, path, n) == 0 && (url[n] == '\0' || url[n] == '?');
}

static void handle_server_info(airplay_server_t *s, rtsp_reply_t *reply) {
    char xml[768];
    int n = snprintf(xml, sizeof(xml),
                     "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                     "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                     "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                     "<plist version=\"1.0\"><dict>\n"
                     "<key>deviceid</key><string>%s</string>\n"
                     "<key>features</key><integer>%llu</integer>\n"
                     "<key>model</key><string>%s</string>\n"
                     "<key>protovers</key><string>1.0</string>\n"
                     "<key>srcvers</key><string>%s</string>\n"
                     "</dict></plist>\n",
                     s->device_id_str, (unsigned long long) s->features, AIRPLAY_MODEL, AIRPLAY_SOURCE_VERSION);
    if (n <= 0 || (size_t) n >= sizeof(xml) ||
        !rtsp_reply_body_copy(reply, "text/x-apple-plist+xml", xml, (size_t) n)) {
        rtsp_reply_status(reply, 500, "Internal Server Error");
    }
}

/* PUT /photo shows a photo, or with X-Apple-AssetAction keeps it for later (cacheOnly) or shows
 * one kept before (displayCached). The body is taken over by the cache instead of copied. */
static void handle_photo(airplay_server_t *s, rtsp_request_t *req, rtsp_reply_t *reply) {
    const char *key = rtsp_header(req, "X-Apple-AssetKey");
    const char *action = rtsp_header(req, "X-Apple-AssetAction");
    if (!key) {
        key = "";
    }
    if (action && str_ieq(action, "displayCached")) {
        photo_t *p = photo_cache_find(s, key);
        if (!p) {
            LOG_I(AIRPLAY, "photo is not cached, the sender will send it again");
            rtsp_reply_status(reply, 412, "Precondition Failed");
            return;
        }
        p->used_ns = time_mono_ns();
        photo_show(s, p->data, p->len);
        return;
    }
    if (!is_jpeg_or_png(req->body, req->body_len)) {
        LOG_W(AIRPLAY, "photo is neither JPEG nor PNG");
        rtsp_reply_status(reply, 400, "Bad Request");
        return;
    }
    if (!(action && str_ieq(action, "cacheOnly"))) {
        photo_show(s, req->body, req->body_len);
    }
    if (photo_cache_adopt(s, key, req->body, req->body_len)) {
        req->body = NULL;
        req->body_len = 0;
    }
}

/* The HTTP/1.1 side of AirPlay, on the same port as RTSP. Only photos are offered; AirPlay video
 * (URL playback) and slideshows are refused and logged, so a refused attempt shows up in the
 * diagnostics. */
static void handle_http(airplay_server_t *s, conn_t *c, rtsp_request_t *req, rtsp_reply_t *reply) {
    const char *m = req->method;
    const char *url = req->url;
    if (s->cfg.require_pin && !c->pair_trusted) {
        LOG_W(AIRPLAY, "HTTP %s %s refused: a PIN is required", m, url);
        rtsp_reply_status(reply, 403, "Forbidden");
        reply->close_connection = true;
    } else if (strcmp(m, "GET") == 0 && path_is(url, "/server-info")) {
        handle_server_info(s, reply);
    } else if (strcmp(m, "POST") == 0 && path_is(url, "/reverse")) {
        /* The sender turns this connection around to receive events. There are none to send, but it
         * stays open for as long as the photo session lasts, and closing it ends the session. */
        rtsp_reply_status(reply, 101, "Switching Protocols");
        rtsp_reply_header(reply, "Upgrade", "PTTH/1.0");
        rtsp_reply_header(reply, "Connection", "Upgrade");
        c->event_channel = true;
    } else if (strcmp(m, "PUT") == 0 && path_is(url, "/photo")) {
        handle_photo(s, req, reply);
    } else if (strcmp(m, "POST") == 0 && path_is(url, "/stop")) {
        photo_session_end(s);
    } else {
        LOG_W(AIRPLAY, "HTTP %s %s is not supported", m, url);
        rtsp_reply_status(reply, 501, "Not Implemented");
        reply->close_connection = true;
    }
}

static void handle_request(airplay_server_t *s, conn_t *c, rtsp_request_t *req, rtsp_reply_t *reply) {
    const char *m = req->method;
    const char *url = req->url;
    bool rtsp = strcmp(req->protocol, "RTSP/1.0") == 0;

    if (!rtsp) {
        handle_http(s, c, req, reply);
        return;
    }
    c->rtsp = true;

    if (strcmp(m, "GET") == 0 && (strcmp(url, "/info") == 0 || strncmp(url, "/info?", 6) == 0)) {
        handle_info(s, c, req, reply);
    } else if (strcmp(m, "POST") == 0) {
        if (strcmp(url, "/pair-setup") == 0) {
            handle_pair_setup(s, c, req, reply);
        } else if (strcmp(url, "/pair-verify") == 0) {
            handle_pair_verify(s, c, req, reply);
        } else if (strcmp(url, "/fp-setup") == 0) {
            handle_fp_setup(s, c, req, reply);
        } else if (strcmp(url, "/pair-pin-start") == 0) {
            handle_pair_pin_start(s, c, req, reply);
        } else if (strcmp(url, "/pair-setup-pin") == 0) {
            handle_pair_setup_pin(s, c, req, reply);
        } else if (strcmp(url, "/feedback") == 0 || strcmp(url, "/audioMode") == 0 ||
                   strcmp(url, "/command") == 0) {
            /* heartbeat and hints: nothing to do beyond noting the activity */
        } else {
            LOG_D(AIRPLAY, "unhandled POST %s", url);
        }
    } else if (strcmp(m, "OPTIONS") == 0) {
        rtsp_reply_header(reply, "Public",
                          "SETUP, RECORD, FLUSH, TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER");
    } else if (strcmp(m, "SETUP") == 0) {
        handle_setup(s, c, req, reply);
    } else if (strcmp(m, "RECORD") == 0) {
        handle_record(s, c, req, reply);
    } else if (strcmp(m, "GET_PARAMETER") == 0) {
        handle_get_parameter(s, c, req, reply);
    } else if (strcmp(m, "SET_PARAMETER") == 0) {
        handle_set_parameter(s, c, req, reply);
    } else if (strcmp(m, "FLUSH") == 0) {
        handle_flush(s, c, req, reply);
    } else if (strcmp(m, "TEARDOWN") == 0) {
        handle_teardown(s, c, req, reply);
    } else {
        rtsp_reply_status(reply, 501, "Not Implemented");
    }
    if (c->session && c->session == s->active) {
        note_remote_control(s, c->session, req);
    }
}

/* Parses and answers every complete request in the receive buffer. Returns false
 * if the connection must be closed. */
static bool process_rx(airplay_server_t *s, conn_t *c) {
    size_t offset = 0;
    bool keep = true;
    while (keep && offset < c->rx_len) {
        rtsp_request_t *req = (rtsp_request_t *) calloc(1, sizeof(rtsp_request_t));
        if (!req) {
            return false;
        }
        size_t consumed = 0;
        rtsp_parse_result_t r = rtsp_parse_request(c->rx + offset, c->rx_len - offset, req, &consumed);
        if (r == RTSP_PARSE_INCOMPLETE) {
            free(req);
            break;
        }
        if (r != RTSP_PARSE_OK) {
            LOG_W(AIRPLAY, "malformed request, closing connection");
            rtsp_request_clear(req);
            free(req);
            return false;
        }
        offset += consumed;

        rtsp_reply_t reply;
        rtsp_reply_init(&reply);
        const char *cseq = rtsp_header(req, "CSeq");
        long long cseq_value = cseq ? parse_uint(cseq, strlen(cseq), 0x7fffffff) : -1;
        if (cseq && cseq_value < 0) {
            rtsp_reply_status(&reply, 400, "Bad Request");
            reply.close_connection = true;
        } else {
            LOG_D(AIRPLAY, "%s %s", req->method, req->url);
            handle_request(s, c, req, &reply);
        }
        if (strcmp(req->protocol, "RTSP/1.0") == 0) {
            if (strcmp(req->method, "RECORD") != 0) {
                rtsp_reply_header(&reply, "Audio-Jack-Status", "connected; type=digital");
            }
            rtsp_reply_header(&reply, "Server", "AirTunes/" AIRPLAY_SOURCE_VERSION);
            if (cseq_value >= 0) {
                char num[16];
                snprintf(num, sizeof(num), "%lld", cseq_value);
                rtsp_reply_header(&reply, "CSeq", num);
            }
        }
        uint8_t *out = NULL;
        size_t out_len = 0;
        if (rtsp_reply_serialize(&reply, req->protocol, &out, &out_len) == 0) {
            if (c->fd >= 0 && net_send_all(c->fd, out, out_len, 3000) != 0) {
                keep = false;
            }
            free(out);
        } else {
            keep = false;
        }
        if (reply.close_connection || c->closing) {
            keep = false;
        }
        rtsp_reply_clear(&reply);
        rtsp_request_clear(req);
        free(req);
    }
    if (offset > 0) {
        memmove(c->rx, c->rx + offset, c->rx_len - offset);
        c->rx_len -= offset;
    }
    c->partial_since_ns = c->rx_len ? (c->partial_since_ns ? c->partial_since_ns : time_mono_ns()) : 0;
    return keep;
}

static bool read_connection(airplay_server_t *s, conn_t *c) {
    for (;;) {
        if (c->rx_len == c->rx_cap) {
            if (c->rx_cap >= RX_LIMIT) {
                LOG_W(AIRPLAY, "request too large");
                return false;
            }
            size_t cap = c->rx_cap ? MIN(c->rx_cap * 2, RX_LIMIT) : 4096;
            uint8_t *p = (uint8_t *) realloc(c->rx, cap);
            if (!p) {
                return false;
            }
            c->rx = p;
            c->rx_cap = cap;
        }
        ssize_t n = recv(c->fd, c->rx + c->rx_len, c->rx_cap - c->rx_len, 0);
        if (n > 0) {
            c->rx_len += (size_t) n;
            c->last_rx_ns = time_mono_ns();
            continue;
        }
        if (n == 0) {
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        return false;
    }
    return process_rx(s, c);
}

/* ------------------------------------------------------------------------- */
/* server thread                                                              */

static uint64_t media_counter(void) {
    return atomic_load_explicit(&g_stats.video_frames_in, memory_order_relaxed) +
           atomic_load_explicit(&g_stats.audio_packets_in, memory_order_relaxed);
}

static void check_timeouts(airplay_server_t *s, uint64_t now) {
    if (s->active) {
        uint64_t count = media_counter();
        if (count != s->last_media_count) {
            s->last_media_count = count;
            s->last_media_ns = now;
        }
    }
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        conn_t *c = s->conns[i];
        if (!c) {
            continue;
        }
        bool close_it = c->closing;
        if (c->partial_since_ns && now - c->partial_since_ns > PARTIAL_REQUEST_TIMEOUT_NS) {
            LOG_W(NETWORK, "incomplete request timed out");
            close_it = true;
        }
        if (c->session) {
            uint64_t last = MAX(c->last_rx_ns, s->last_media_ns);
            if (now - last > SESSION_TIMEOUT_NS) {
                LOG_W(SESSION, "sender stopped responding, ending session");
                close_it = true;
            }
        } else if (c->event_channel) {
            if (now - c->created_ns > EVENT_CHANNEL_TIMEOUT_NS) {
                close_it = true;
            }
        } else if (now - c->last_rx_ns > IDLE_TIMEOUT_NS) {
            close_it = true;
        }
        if (close_it) {
            conn_close(s, i);
        }
    }
    if (s->pin_active && now - s->pin_since_ns > PIN_TIMEOUT_NS) {
        hide_pin(s);
    }
}

static void *server_thread(void *arg) {
    airplay_server_t *s = (airplay_server_t *) arg;
    struct pollfd pfds[3 + MAX_CONNECTIONS];
    conn_t *polled[MAX_CONNECTIONS];

    while (atomic_load(&s->running)) {
        int n = 0;
        pfds[n++] = (struct pollfd) { .fd = s->wake.rd, .events = POLLIN };
        for (int i = 0; i < 2; i++) {
            if (s->listen_fd[i] >= 0) {
                pfds[n++] = (struct pollfd) { .fd = s->listen_fd[i], .events = POLLIN };
            }
        }
        int first_conn = n;
        int conn_count = 0;
        for (int i = 0; i < MAX_CONNECTIONS; i++) {
            if (s->conns[i]) {
                polled[conn_count++] = s->conns[i];
                pfds[n++] = (struct pollfd) { .fd = s->conns[i]->fd, .events = POLLIN };
            }
        }
        /* Timers only run while there is something to supervise; idle means no wakeups. */
        int timeout_ms = (conn_count > 0 || s->pin_active) ? 2000 : -1;
        int r = poll(pfds, (nfds_t) n, timeout_ms);
        if (r < 0 && errno != EINTR) {
            LOG_E(NETWORK, "server poll failed: %d", errno);
            break;
        }
        if (!atomic_load(&s->running)) {
            break;
        }
        if (pfds[0].revents & POLLIN) {
            wakeup_drain(&s->wake);
        }
        if (atomic_exchange(&s->disconnect_requested, false) && s->active) {
            conn_t *owner = s->active->owner;
            LOG_I(SESSION, "session ended on the receiver");
            session_destroy(s, s->active);
            if (owner) {
                owner->closing = true;
            }
        }
        if (s->active && atomic_load(&s->active->stream_lost)) {
            conn_t *owner = s->active->owner;
            session_destroy(s, s->active);
            if (owner) {
                owner->closing = true;
            }
        }
        for (int k = 0; k < conn_count; k++) {
            conn_t *c = polled[k];
            int idx = conn_index(s, c);
            if (idx < 0) {
                continue; /* closed meanwhile */
            }
            short ev = pfds[first_conn + k].revents;
            if (c->closing) {
                conn_close(s, idx);
                continue;
            }
            if (ev & (POLLIN | POLLHUP | POLLERR)) {
                if (!read_connection(s, c)) {
                    idx = conn_index(s, c);
                    if (idx >= 0) {
                        conn_close(s, idx);
                    }
                }
            }
        }
        for (int i = 1; i < first_conn; i++) {
            if (pfds[i].revents & POLLIN) {
                accept_connections(s, pfds[i].fd);
            }
        }
        check_timeouts(s, time_mono_ns());
    }

    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        conn_close(s, i);
    }
    hide_pin(s);
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* public API                                                                 */

airplay_server_t *airplay_server_create(const airplay_config_t *config, const airplay_events_t *events,
                                        const media_sink_ops_t *media, void *media_ctx) {
    if (!config || !media) {
        return NULL;
    }
    airplay_server_t *s = (airplay_server_t *) calloc(1, sizeof(airplay_server_t));
    if (!s) {
        return NULL;
    }
    s->cfg = *config;
    if (events) {
        s->ev = *events;
    }
    s->media = media;
    s->media_ctx = media_ctx;
    s->listen_fd[0] = s->listen_fd[1] = -1;
    s->wake.rd = s->wake.wr = -1;
    s->volume_db = 0.0f;
    pthread_mutex_init(&s->paired_lock, NULL);

    if (s->cfg.display_width < 320 || s->cfg.display_height < 240) {
        s->cfg.display_width = 1920;
        s->cfg.display_height = 1080;
    }
    if (s->cfg.display_fps <= 0 || s->cfg.display_fps > 120) {
        s->cfg.display_fps = 60;
    }
    if (!s->cfg.name[0]) {
        str_copy(s->cfg.name, sizeof(s->cfg.name), "AirPlay TV");
    }
    pairing_identity_init(&s->identity, config->identity_seed);
    secure_zero(s->cfg.identity_seed, sizeof(s->cfg.identity_seed));
    hex_encode(s->identity.public_key, ED25519_KEY_SIZE, s->pk_hex);
    format_device_id(s->cfg.device_id, s->device_id_str);
    s->features = airplay_features(&s->cfg);
    return s;
}

int airplay_server_start(airplay_server_t *s) {
    if (!s || s->thread_started) {
        return -1;
    }
    uint16_t port = s->cfg.port;
    s->listen_fd[0] = net_listen_tcp(AF_INET, &port, 8);
    if (s->listen_fd[0] < 0 && port != 0) {
        LOG_W(NETWORK, "port %u is busy, using another port", s->cfg.port);
        port = 0;
        s->listen_fd[0] = net_listen_tcp(AF_INET, &port, 8);
    }
    if (s->listen_fd[0] < 0) {
        LOG_E(NETWORK, "cannot listen for AirPlay connections: %d", errno);
        return -1;
    }
    uint16_t port6 = port;
    s->listen_fd[1] = net_listen_tcp(AF_INET6, &port6, 8);
    if (s->listen_fd[1] < 0) {
        LOG_W(NETWORK, "IPv6 is not available");
    }
    for (int i = 0; i < 2; i++) {
        if (s->listen_fd[i] >= 0) {
            net_set_nonblocking(s->listen_fd[i], true);
        }
    }
    if (wakeup_init(&s->wake) != 0) {
        airplay_server_stop(s);
        return -1;
    }
    s->port = port;
    atomic_store(&s->running, true);
    if (pthread_create(&s->thread, NULL, server_thread, s) != 0) {
        atomic_store(&s->running, false);
        airplay_server_stop(s);
        return -1;
    }
    s->thread_started = true;
    LOG_I(AIRPLAY, "receiver listening on port %u", port);
    return port;
}

void airplay_server_stop(airplay_server_t *s) {
    if (!s) {
        return;
    }
    atomic_store(&s->running, false);
    if (s->thread_started) {
        wakeup_signal(&s->wake);
        pthread_join(s->thread, NULL);
        s->thread_started = false;
    }
    for (int i = 0; i < 2; i++) {
        net_close(&s->listen_fd[i]);
    }
    wakeup_close(&s->wake);
}

void airplay_server_destroy(airplay_server_t *s) {
    if (!s) {
        return;
    }
    airplay_server_stop(s);
    photo_cache_clear(s);
    airplay_server_set_paired_clients(s, NULL, 0);
    pthread_mutex_destroy(&s->paired_lock);
    secure_zero(&s->identity, sizeof(s->identity));
    free(s);
}

void airplay_server_disconnect(airplay_server_t *s) {
    if (s) {
        atomic_store(&s->disconnect_requested, true);
        wakeup_signal(&s->wake);
    }
}

bool airplay_server_session_active(airplay_server_t *s) {
    return s && atomic_load(&s->session_active);
}

#ifdef AIRPLAYTV_FUZZING
/* Feeds raw bytes through the request handlers as if they came from a LAN client. */
void airplay_server_fuzz_input(airplay_server_t *s, const uint8_t *data, size_t len) {
    conn_t *c = (conn_t *) calloc(1, sizeof(conn_t));
    if (!c) {
        return;
    }
    c->fd = -1;
    struct sockaddr_in *peer = (struct sockaddr_in *) &c->peer;
    peer->sin_family = AF_INET;
    peer->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    c->created_ns = c->last_rx_ns = time_mono_ns();
    pairing_session_init(&c->pair);
    fairplay_reset(&c->fp);
    c->rx = (uint8_t *) malloc(len ? len : 1);
    if (c->rx) {
        memcpy(c->rx, data, len);
        c->rx_len = len;
        c->rx_cap = len;
    }
    s->conns[0] = c;
    process_rx(s, c);
    conn_close(s, 0);
}
#endif
