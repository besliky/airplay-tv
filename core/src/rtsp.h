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
 * Strict parser for the RTSP/1.0 and HTTP/1.1 requests an AirPlay sender makes,
 * and a builder for the matching responses. Requests must use CRLF line endings
 * and a Content-Length for bodies; everything else is rejected.
 */

#ifndef AIRPLAYTV_RTSP_H
#define AIRPLAYTV_RTSP_H

#include "common.h"

#define RTSP_MAX_HEAD 16384
#define RTSP_MAX_BODY (2 * 1024 * 1024)
#define RTSP_MAX_PUT_BODY (16 * 1024 * 1024) /* PUT /photo: a full-size picture from the Photos app */
#define RTSP_MAX_HEADERS 32
#define RTSP_MAX_NAME 48
#define RTSP_MAX_VALUE 640

typedef struct {
    char name[RTSP_MAX_NAME];
    char value[RTSP_MAX_VALUE];
} rtsp_header_t;

typedef struct {
    char method[32];
    char url[256];
    char protocol[16];
    int header_count;
    rtsp_header_t headers[RTSP_MAX_HEADERS];
    uint8_t *body;
    size_t body_len;
} rtsp_request_t;

typedef enum {
    RTSP_PARSE_INCOMPLETE = 0,
    RTSP_PARSE_OK = 1,
    RTSP_PARSE_BAD = -1,
    RTSP_PARSE_TOO_LARGE = -2,
} rtsp_parse_result_t;

/* Parses one request from the start of buf. On RTSP_PARSE_OK, *consumed is the
 * number of bytes the request occupied and req owns a copy of the body. */
rtsp_parse_result_t rtsp_parse_request(const uint8_t *buf, size_t len, rtsp_request_t *req, size_t *consumed);
void rtsp_request_clear(rtsp_request_t *req);

/* Case-insensitive header lookup. */
const char *rtsp_header(const rtsp_request_t *req, const char *name);

#define RTSP_REPLY_MAX_HEADERS 12

typedef struct {
    int status;
    const char *reason;
    int header_count;
    struct {
        char name[40];
        char value[200];
    } headers[RTSP_REPLY_MAX_HEADERS];
    const char *content_type;
    uint8_t *body;          /* malloc'd, freed by rtsp_reply_clear */
    size_t body_len;
    bool close_connection;
} rtsp_reply_t;

void rtsp_reply_init(rtsp_reply_t *reply);
void rtsp_reply_status(rtsp_reply_t *reply, int status, const char *reason);
void rtsp_reply_header(rtsp_reply_t *reply, const char *name, const char *value);
/* Takes ownership of body (must be malloc'd). */
void rtsp_reply_body(rtsp_reply_t *reply, const char *content_type, uint8_t *body, size_t len);
/* Copies data into a new body. */
bool rtsp_reply_body_copy(rtsp_reply_t *reply, const char *content_type, const void *data, size_t len);
void rtsp_reply_clear(rtsp_reply_t *reply);

/* Serializes the reply. *out is malloc'd. */
int rtsp_reply_serialize(const rtsp_reply_t *reply, const char *protocol, uint8_t **out, size_t *out_len);

#endif
