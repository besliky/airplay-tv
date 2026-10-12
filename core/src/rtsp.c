/*
 * AirPlay TV - AirPlay screen mirroring receiver for Android TV
 * Copyright (C) 2026 besliky
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "rtsp.h"

#include <stdio.h>
#include <stdlib.h>

#include "util.h"

static bool is_token_char(uint8_t c) {
    /* RFC 7230 tchar */
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        return true;
    }
    return c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static const uint8_t *find_head_end(const uint8_t *buf, size_t len) {
    for (size_t i = 3; i < len; i++) {
        if (buf[i] == '\n' && buf[i - 1] == '\r' && buf[i - 2] == '\n' && buf[i - 3] == '\r') {
            return buf + i + 1;
        }
    }
    return NULL;
}

/* Copies [start, end) into dst after trimming spaces and tabs. */
static bool copy_trimmed(char *dst, size_t cap, const uint8_t *start, const uint8_t *end) {
    while (start < end && (*start == ' ' || *start == '\t')) {
        start++;
    }
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    size_t n = (size_t) (end - start);
    if (n >= cap) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        /* header values must not contain control characters other than tab */
        if ((start[i] < 0x20 && start[i] != '\t') || start[i] == 0x7f) {
            return false;
        }
    }
    memcpy(dst, start, n);
    dst[n] = '\0';
    return true;
}

static bool parse_request_line(const uint8_t *line, const uint8_t *end, rtsp_request_t *req) {
    const uint8_t *p = line;
    const uint8_t *sp1 = NULL;
    const uint8_t *sp2 = NULL;
    for (const uint8_t *q = p; q < end; q++) {
        if (*q == ' ') {
            if (!sp1) {
                sp1 = q;
            } else if (!sp2) {
                sp2 = q;
            } else {
                return false;
            }
        }
    }
    if (!sp1 || !sp2 || sp1 == p || sp2 == sp1 + 1 || sp2 + 1 >= end) {
        return false;
    }
    size_t mlen = (size_t) (sp1 - p);
    size_t ulen = (size_t) (sp2 - sp1 - 1);
    size_t plen = (size_t) (end - sp2 - 1);
    if (mlen >= sizeof(req->method) || ulen >= sizeof(req->url) || plen >= sizeof(req->protocol)) {
        return false;
    }
    for (size_t i = 0; i < mlen; i++) {
        if (!is_token_char(p[i])) {
            return false;
        }
    }
    for (size_t i = 0; i < ulen; i++) {
        uint8_t c = sp1[1 + i];
        if (c <= 0x20 || c >= 0x7f) {
            return false;
        }
    }
    memcpy(req->method, p, mlen);
    req->method[mlen] = '\0';
    memcpy(req->url, sp1 + 1, ulen);
    req->url[ulen] = '\0';
    memcpy(req->protocol, sp2 + 1, plen);
    req->protocol[plen] = '\0';
    return strcmp(req->protocol, "RTSP/1.0") == 0 || strcmp(req->protocol, "HTTP/1.1") == 0 ||
           strcmp(req->protocol, "HTTP/1.0") == 0;
}

rtsp_parse_result_t rtsp_parse_request(const uint8_t *buf, size_t len, rtsp_request_t *req, size_t *consumed) {
    const uint8_t *head_end = find_head_end(buf, MIN(len, (size_t) RTSP_MAX_HEAD));
    if (!head_end) {
        return len >= RTSP_MAX_HEAD ? RTSP_PARSE_TOO_LARGE : RTSP_PARSE_INCOMPLETE;
    }
    memset(req, 0, sizeof(*req));

    const uint8_t *line = buf;
    const uint8_t *limit = head_end - 2; /* points at the final CRLF */
    bool first = true;
    long long content_length = 0;
    bool have_length = false;

    while (line < limit) {
        const uint8_t *eol = line;
        while (eol + 1 < head_end && !(eol[0] == '\r' && eol[1] == '\n')) {
            eol++;
        }
        if (eol + 1 >= head_end) {
            return RTSP_PARSE_BAD;
        }
        if (first) {
            if (!parse_request_line(line, eol, req)) {
                return RTSP_PARSE_BAD;
            }
            first = false;
        } else {
            const uint8_t *colon = memchr(line, ':', (size_t) (eol - line));
            if (!colon || colon == line || req->header_count >= RTSP_MAX_HEADERS) {
                return RTSP_PARSE_BAD;
            }
            for (const uint8_t *q = line; q < colon; q++) {
                if (!is_token_char(*q)) {
                    return RTSP_PARSE_BAD;
                }
            }
            rtsp_header_t *h = &req->headers[req->header_count];
            if ((size_t) (colon - line) >= sizeof(h->name)) {
                return RTSP_PARSE_BAD;
            }
            memcpy(h->name, line, (size_t) (colon - line));
            h->name[colon - line] = '\0';
            if (!copy_trimmed(h->value, sizeof(h->value), colon + 1, eol)) {
                return RTSP_PARSE_BAD;
            }
            if (str_ieq(h->name, "Content-Length")) {
                if (have_length) {
                    return RTSP_PARSE_BAD;
                }
                long long max_body = strcmp(req->method, "PUT") == 0 ? RTSP_MAX_PUT_BODY : RTSP_MAX_BODY;
                content_length = parse_uint(h->value, strlen(h->value), max_body);
                if (content_length < 0) {
                    return RTSP_PARSE_TOO_LARGE;
                }
                have_length = true;
            } else if (str_ieq(h->name, "Transfer-Encoding")) {
                return RTSP_PARSE_BAD;
            }
            req->header_count++;
        }
        line = eol + 2;
    }
    if (first) {
        return RTSP_PARSE_BAD;
    }

    size_t head_len = (size_t) (head_end - buf);
    if (len - head_len < (size_t) content_length) {
        return RTSP_PARSE_INCOMPLETE;
    }
    if (content_length > 0) {
        req->body = (uint8_t *) malloc((size_t) content_length);
        if (!req->body) {
            return RTSP_PARSE_TOO_LARGE;
        }
        memcpy(req->body, head_end, (size_t) content_length);
        req->body_len = (size_t) content_length;
    }
    *consumed = head_len + (size_t) content_length;
    return RTSP_PARSE_OK;
}

void rtsp_request_clear(rtsp_request_t *req) {
    if (req) {
        free(req->body);
        req->body = NULL;
        req->body_len = 0;
    }
}

const char *rtsp_header(const rtsp_request_t *req, const char *name) {
    for (int i = 0; i < req->header_count; i++) {
        if (str_ieq(req->headers[i].name, name)) {
            return req->headers[i].value;
        }
    }
    return NULL;
}

void rtsp_reply_init(rtsp_reply_t *reply) {
    memset(reply, 0, sizeof(*reply));
    reply->status = 200;
    reply->reason = "OK";
}

void rtsp_reply_status(rtsp_reply_t *reply, int status, const char *reason) {
    reply->status = status;
    reply->reason = reason;
}

void rtsp_reply_header(rtsp_reply_t *reply, const char *name, const char *value) {
    for (int i = 0; i < reply->header_count; i++) {
        if (str_ieq(reply->headers[i].name, name)) {
            str_copy(reply->headers[i].value, sizeof(reply->headers[i].value), value);
            return;
        }
    }
    if (reply->header_count >= RTSP_REPLY_MAX_HEADERS) {
        return;
    }
    str_copy(reply->headers[reply->header_count].name, sizeof(reply->headers[0].name), name);
    str_copy(reply->headers[reply->header_count].value, sizeof(reply->headers[0].value), value);
    reply->header_count++;
}

void rtsp_reply_body(rtsp_reply_t *reply, const char *content_type, uint8_t *body, size_t len) {
    free(reply->body);
    reply->body = body;
    reply->body_len = body ? len : 0;
    reply->content_type = content_type;
}

bool rtsp_reply_body_copy(rtsp_reply_t *reply, const char *content_type, const void *data, size_t len) {
    uint8_t *copy = NULL;
    if (len) {
        copy = (uint8_t *) malloc(len);
        if (!copy) {
            return false;
        }
        memcpy(copy, data, len);
    }
    rtsp_reply_body(reply, content_type, copy, len);
    return true;
}

void rtsp_reply_clear(rtsp_reply_t *reply) {
    free(reply->body);
    reply->body = NULL;
    reply->body_len = 0;
}

int rtsp_reply_serialize(const rtsp_reply_t *reply, const char *protocol, uint8_t **out, size_t *out_len) {
    size_t cap = 256 + reply->body_len;
    for (int i = 0; i < reply->header_count; i++) {
        cap += strlen(reply->headers[i].name) + strlen(reply->headers[i].value) + 4;
    }
    if (reply->content_type) {
        cap += strlen(reply->content_type) + 16;
    }
    char *buf = (char *) malloc(cap);
    if (!buf) {
        return -1;
    }
    int status = (reply->status >= 100 && reply->status <= 999) ? reply->status : 500;
    size_t n = (size_t) snprintf(buf, cap, "%s %d %s\r\n", protocol ? protocol : "RTSP/1.0", status,
                                 reply->reason ? reply->reason : "OK");
    for (int i = 0; i < reply->header_count && n < cap; i++) {
        n += (size_t) snprintf(buf + n, cap - n, "%s: %s\r\n", reply->headers[i].name, reply->headers[i].value);
    }
    if (reply->content_type && n < cap) {
        n += (size_t) snprintf(buf + n, cap - n, "Content-Type: %s\r\n", reply->content_type);
    }
    /* An HTTP connection stays open after the answer, so every final answer states its length. */
    bool http = protocol && strncmp(protocol, "HTTP/", 5) == 0;
    if ((reply->body_len || reply->content_type || (http && status >= 200)) && n < cap) {
        n += (size_t) snprintf(buf + n, cap - n, "Content-Length: %zu\r\n", reply->body_len);
    }
    if (n + 2 + reply->body_len > cap) {
        free(buf);
        return -1;
    }
    buf[n++] = '\r';
    buf[n++] = '\n';
    if (reply->body_len) {
        memcpy(buf + n, reply->body, reply->body_len);
        n += reply->body_len;
    }
    *out = (uint8_t *) buf;
    *out_len = n;
    return 0;
}
