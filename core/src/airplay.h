/*
 * AirPlay TV - AirPlay screen mirroring receiver for Android TV
 * Copyright (C) 2026 besliky
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef AIRPLAYTV_AIRPLAY_H
#define AIRPLAYTV_AIRPLAY_H

#include "common.h"
#include "media.h"

#define AIRPLAY_MODEL "AppleTV3,2"
#define AIRPLAY_SOURCE_VERSION "220.68"
#define AIRPLAY_DEFAULT_PORT 7000

typedef struct airplay_server airplay_server_t;

typedef struct {
    char name[64];              /* UTF-8, shown in the sender's device list */
    uint8_t device_id[6];       /* stable random id, advertised like a MAC address */
    char public_id[40];         /* stable random UUID ("pi") */
    uint8_t identity_seed[32];  /* Ed25519 seed of the receiver identity */
    bool require_pin;
    uint16_t port;              /* preferred RTSP port, 0 for any */
    int display_width;
    int display_height;
    int display_fps;
    bool hevc;                  /* advertise H.265 mirroring */
} airplay_config_t;

typedef struct {
    void *ctx;
    /* A sender established a session (first SETUP). */
    void (*session_started)(void *ctx, const char *client_name, const char *client_model);
    void (*session_ended)(void *ctx);
    /* Show a PIN for pairing; NULL hides it again. */
    void (*pin_display)(void *ctx, const char *pin);
    /* A sender completed PIN pairing and should be remembered. */
    void (*client_paired)(void *ctx, const char *client_key_b64, const char *client_name);

    /* The callbacks below are optional. They describe the active session only. */

    /* Track info pushed by the sender. Strings are UTF-8 and may be empty. */
    void (*track_info)(void *ctx, const char *title, const char *artist, const char *album);
    /* Cover art as an encoded image, valid during the call; NULL when the track has none. */
    void (*artwork)(void *ctx, const uint8_t *data, size_t len);
    /* Playback position in RTP timestamp units of the audio stream. */
    void (*progress)(void *ctx, uint32_t start, uint32_t current, uint32_t end);
    /* The sender's remote-control (DACP) service id and the token every command must carry.
     * Reported when they first appear and whenever they change. */
    void (*remote_control)(void *ctx, const char *dacp_id, const char *active_remote);

    /* A photo from the sender's Photos app (JPEG or PNG), valid during the call. */
    void (*photo)(void *ctx, const uint8_t *data, size_t len);
    /* The sender ended the photo session. */
    void (*photo_stop)(void *ctx);
} airplay_events_t;

typedef struct {
    char key[16];
    char value[160];
} txt_entry_t;

airplay_server_t *airplay_server_create(const airplay_config_t *config, const airplay_events_t *events,
                                        const media_sink_ops_t *media, void *media_ctx);
/* Binds the RTSP port and starts the server thread. Returns the port or -1. */
int airplay_server_start(airplay_server_t *s);
void airplay_server_stop(airplay_server_t *s);
void airplay_server_destroy(airplay_server_t *s);

/* Ends the current session from the receiver side (e.g. the user pressed Back). */
void airplay_server_disconnect(airplay_server_t *s);

/* Replaces the list of remembered PIN-paired senders (base64 Ed25519 keys). */
void airplay_server_set_paired_clients(airplay_server_t *s, const char *const *keys, int count);

bool airplay_server_session_active(airplay_server_t *s);

uint64_t airplay_features(const airplay_config_t *config);
void airplay_public_key_hex(const airplay_server_t *s, char out[65]);

/* Service instance name of the RAOP record: "<DEVICEID>@<name>". */
void airplay_raop_name(const airplay_config_t *config, char *out, size_t cap);

/* TXT records of the _airplay._tcp and _raop._tcp services. Return the entry count. */
int airplay_txt_airplay(const airplay_server_t *s, txt_entry_t *out, int max);
int airplay_txt_raop(const airplay_server_t *s, txt_entry_t *out, int max);

/* DNS TXT wire encoding (length-prefixed "key=value" strings). */
size_t airplay_txt_encode(const txt_entry_t *entries, int count, uint8_t *out, size_t cap);

#ifdef AIRPLAYTV_FUZZING
void airplay_server_fuzz_input(airplay_server_t *s, const uint8_t *data, size_t len);
#endif

#endif
