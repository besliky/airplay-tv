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
 * A minimal AirPlay mirroring sender for tests. It speaks the same RTSP,
 * pairing, FairPlay-framing and stream formats as an iOS device. The FairPlay
 * key exchange cannot be reproduced without Apple's implementation, so the
 * sender derives the stream key with the receiver's own playfair code, which
 * makes both ends agree on the key.
 */

#ifndef AIRPLAYTV_FAKE_SENDER_H
#define AIRPLAYTV_FAKE_SENDER_H

#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>

#include "crypto.h"

typedef struct {
    int fd;
    struct sockaddr_in server;
    int cseq;
    char user_agent[64];
    char extra_headers[160];  /* "Name: value\r\n" lines added to every RTSP request */

    uint8_t ed_secret[ED25519_SECRET_SIZE];
    uint8_t ed_public[ED25519_KEY_SIZE];
    uint8_t server_ed[ED25519_KEY_SIZE];
    uint8_t shared[X25519_KEY_SIZE];
    bool have_shared;

    uint8_t keymsg[164];
    uint8_t aeskey[16];
    uint8_t aesiv[16];
    bool session;

    int timing_fd;
    uint16_t timing_port;

    int mirror_fd;
    aes_ctr_t ctr;
    bool ctr_ready;

    int audio_fd;
    int control_fd;
    uint16_t control_port;
    struct sockaddr_in audio_data;
    struct sockaddr_in audio_control;
    uint16_t audio_seq;
    uint32_t audio_rtp;
} fake_sender_t;

int fs_connect(fake_sender_t *f, const char *ip, uint16_t port);
void fs_close(fake_sender_t *f);

/* Sends one RTSP request and reads the response. *body is malloc'd (may be NULL). */
int fs_request(fake_sender_t *f, const char *method, const char *url, const char *content_type,
               const void *data, size_t len, int *status, uint8_t **body, size_t *body_len);

int fs_info(fake_sender_t *f, uint8_t server_pk[32]);
int fs_pair(fake_sender_t *f);
/* PIN pairing: pin_source is called after /pair-pin-start and must return the PIN shown. */
int fs_pair_pin(fake_sender_t *f, const char *(*pin_source)(void *ctx), void *ctx);
int fs_fairplay(fake_sender_t *f);
/* First SETUP. Returns the RTSP status. */
int fs_setup_session(fake_sender_t *f, const char *name, const char *model);
int fs_setup_mirror(fake_sender_t *f, uint64_t stream_connection_id);
int fs_setup_audio(fake_sender_t *f, int ct, int spf);
int fs_send_codec(fake_sender_t *f, const uint8_t *config, size_t len, int width, int height, uint8_t option);
int fs_send_video(fake_sender_t *f, const uint8_t *avcc_au, size_t len, uint64_t ntp_timestamp);
int fs_send_audio(fake_sender_t *f, const uint8_t *payload, size_t len, uint32_t samples);
int fs_send_sync(fake_sender_t *f);
int fs_set_volume(fake_sender_t *f, float db);
/* SET_PARAMETER with any content type. Returns 0 on a 200 reply. */
int fs_set_parameter(fake_sender_t *f, const char *content_type, const void *data, size_t len);
/* Track info (DMAP) and playback position in RTP timestamp units, as a music app sends them. */
int fs_set_track(fake_sender_t *f, const char *title, const char *artist, const char *album);
int fs_set_progress(fake_sender_t *f, uint32_t start, uint32_t current, uint32_t end);
/* One HTTP/1.1 request as the Photos app sends it; headers are "Name: value\r\n" lines or NULL.
 * Returns the reply status or -1. The reply head goes to reply_head when it is not NULL, the body
 * to *body (malloc'd) when body is not NULL. */
int fs_http(fake_sender_t *f, const char *method, const char *url, const char *headers, const void *data,
            size_t len, char *reply_head, size_t head_cap, uint8_t **body, size_t *body_len);
int fs_feedback(fake_sender_t *f);
/* type 0 tears down the whole session. */
int fs_teardown(fake_sender_t *f, int type);

/* Uncompressed ALAC frame with a stereo sine wave (352 samples, 16 bit). */
size_t fs_make_alac_frame(uint8_t *out, size_t cap, int phase);

#endif
