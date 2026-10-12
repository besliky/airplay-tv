/*
 * AirPlay TV - AirPlay screen mirroring receiver for Android TV
 * Copyright (C) 2026 besliky
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <stdlib.h>

#include "bplist.h"
#include "dmap.h"
#include "rtsp.h"
#include "test.h"
#include "util.h"

/* ---- binary plist ---- */

TEST(bplist_roundtrip) {
    bp_node_t *root = bp_new_dict();
    bp_dict_set(root, "name", bp_new_string("Living Room TV"));
    bp_dict_set(root, "unicode", bp_new_string("Гостиная 📺"));
    bp_dict_set(root, "features", bp_new_uint(0x5A7FFEE6ULL | (1ULL << 42)));
    bp_dict_set(root, "negative", bp_new_int(-5));
    bp_dict_set(root, "rate", bp_new_real(1.0 / 60.0));
    bp_dict_set(root, "flag", bp_new_bool(true));
    uint8_t blob[300];
    for (int i = 0; i < 300; i++) blob[i] = (uint8_t) i;
    bp_dict_set(root, "blob", bp_new_data(blob, sizeof(blob)));
    bp_node_t *arr = bp_new_array();
    for (int i = 0; i < 20; i++) bp_array_append(arr, bp_new_uint((uint64_t) i * 1000));
    bp_dict_set(root, "list", arr);

    uint8_t *data = NULL;
    size_t len = 0;
    CHECK(bp_write(root, &data, &len) == 0);
    bp_free(root);

    bp_node_t *back = bp_parse(data, len);
    free(data);
    CHECK(back != NULL);
    CHECK_STR(bp_get_string(bp_dict_get(back, "name")), "Living Room TV");
    CHECK_STR(bp_get_string(bp_dict_get(back, "unicode")), "Гостиная 📺");
    uint64_t u = 0;
    CHECK(bp_get_uint(bp_dict_get(back, "features"), &u));
    CHECK(u == (0x5A7FFEE6ULL | (1ULL << 42)));
    int64_t neg = 0;
    CHECK(bp_get_int(bp_dict_get(back, "negative"), &neg));
    CHECK_EQ(neg, -5);
    CHECK(!bp_get_uint(bp_dict_get(back, "negative"), &u));
    double r = 0;
    CHECK(bp_get_real(bp_dict_get(back, "rate"), &r));
    CHECK(r > 0.0166 && r < 0.0167);
    bool b = false;
    CHECK(bp_get_bool(bp_dict_get(back, "flag"), &b) && b);
    size_t blob_len = 0;
    const uint8_t *blob2 = bp_get_data(bp_dict_get(back, "blob"), &blob_len);
    CHECK(blob2 && blob_len == sizeof(blob) && memcmp(blob, blob2, blob_len) == 0);
    CHECK_EQ(bp_count(bp_dict_get(back, "list")), 20);
    CHECK(bp_get_uint(bp_array_get(bp_dict_get(back, "list"), 19), &u) && u == 19000);
    CHECK(bp_dict_get(back, "missing") == NULL);
    CHECK(bp_get_string(bp_dict_get(back, "features")) == NULL);
    bp_free(back);
}

TEST(bplist_rejects_garbage) {
    uint8_t junk[64];
    memset(junk, 0xff, sizeof(junk));
    CHECK(bp_parse(junk, sizeof(junk)) == NULL);
    CHECK(bp_parse((const uint8_t *) "bplist00", 8) == NULL);
    CHECK(bp_parse(NULL, 0) == NULL);

    bp_node_t *root = bp_new_dict();
    bp_dict_set(root, "k", bp_new_string("value"));
    uint8_t *data = NULL;
    size_t len = 0;
    CHECK(bp_write(root, &data, &len) == 0);
    bp_free(root);
    /* every truncation and every single-byte corruption must be handled */
    for (size_t cut = 0; cut < len; cut++) {
        bp_free(bp_parse(data, cut));
    }
    for (size_t i = 0; i < len; i++) {
        for (int v = 0; v < 256; v += 17) {
            uint8_t saved = data[i];
            data[i] = (uint8_t) v;
            bp_free(bp_parse(data, len));
            data[i] = saved;
        }
    }
    free(data);
}

TEST(bplist_reference_cycle_is_bounded) {
    /* an array whose only element is itself */
    uint8_t p[] = {
        'b', 'p', 'l', 'i', 's', 't', '0', '0',
        0xa1, 0x00,                                 /* object 0: array [ref 0] */
        0x08,                                       /* offset table: object 0 at 8 */
        0, 0, 0, 0, 0, 0,                           /* trailer */
        1, 1,                                       /* offset size, ref size */
        0, 0, 0, 0, 0, 0, 0, 1,                     /* one object */
        0, 0, 0, 0, 0, 0, 0, 0,                     /* top object 0 */
        0, 0, 0, 0, 0, 0, 0, 10,                    /* offset table at 10 */
    };
    CHECK(bp_parse(p, sizeof(p)) == NULL);
}

TEST(bplist_exponential_sharing_is_bounded) {
    /* 40 arrays, each referencing the next one twice: 2^40 nodes if expanded */
    uint8_t buf[512];
    size_t o = 0;
    memcpy(buf, "bplist00", 8);
    o = 8;
    uint8_t offsets[41];
    for (int i = 0; i < 40; i++) {
        offsets[i] = (uint8_t) o;
        buf[o++] = 0xa2;
        buf[o++] = (uint8_t) (i + 1);
        buf[o++] = (uint8_t) (i + 1);
    }
    offsets[40] = (uint8_t) o;
    buf[o++] = 0x09; /* true */
    size_t table = o;
    memcpy(buf + o, offsets, 41);
    o += 41;
    memset(buf + o, 0, 32);
    buf[o + 6] = 1;
    buf[o + 7] = 1;
    buf[o + 15] = 41;
    buf[o + 31] = (uint8_t) table;
    o += 32;
    CHECK(bp_parse(buf, o) == NULL);
}

/* ---- RTSP ---- */

TEST(rtsp_parses_request_with_body) {
    const char *msg = "SETUP rtsp://192.168.1.10/123 RTSP/1.0\r\nCSeq: 7\r\nContent-Type: application/x-apple-binary-plist\r\n"
                      "content-length: 4\r\nUser-Agent:   AirPlay/860.7.1  \r\n\r\nabcdEXTRA";
    rtsp_request_t req;
    size_t used = 0;
    CHECK_EQ(rtsp_parse_request((const uint8_t *) msg, strlen(msg), &req, &used), RTSP_PARSE_OK);
    CHECK_STR(req.method, "SETUP");
    CHECK_STR(req.url, "rtsp://192.168.1.10/123");
    CHECK_STR(req.protocol, "RTSP/1.0");
    CHECK_STR(rtsp_header(&req, "cseq"), "7");
    CHECK_STR(rtsp_header(&req, "User-Agent"), "AirPlay/860.7.1");
    CHECK_EQ(req.body_len, 4);
    CHECK_MEM(req.body, "abcd", 4);
    CHECK_EQ(used, strlen(msg) - 5);
    rtsp_request_clear(&req);
}

TEST(rtsp_incomplete_and_pipelined) {
    const char *two = "POST /feedback RTSP/1.0\r\nCSeq: 1\r\n\r\nGET /info RTSP/1.0\r\nCSeq: 2\r\n\r\n";
    rtsp_request_t req;
    size_t used = 0;
    for (size_t cut = 0; cut < 36; cut++) {
        CHECK_EQ(rtsp_parse_request((const uint8_t *) two, cut, &req, &used), RTSP_PARSE_INCOMPLETE);
    }
    CHECK_EQ(rtsp_parse_request((const uint8_t *) two, strlen(two), &req, &used), RTSP_PARSE_OK);
    CHECK_STR(req.url, "/feedback");
    rtsp_request_clear(&req);
    size_t first = used;
    CHECK_EQ(rtsp_parse_request((const uint8_t *) two + first, strlen(two) - first, &req, &used), RTSP_PARSE_OK);
    CHECK_STR(req.url, "/info");
    rtsp_request_clear(&req);

    const char *body_pending = "POST /fp-setup RTSP/1.0\r\nContent-Length: 16\r\n\r\n12345678";
    CHECK_EQ(rtsp_parse_request((const uint8_t *) body_pending, strlen(body_pending), &req, &used),
             RTSP_PARSE_INCOMPLETE);
}

TEST(rtsp_rejects_malformed) {
    const char *bad[] = {
        "GARBAGE\r\n\r\n",
        "GET /info\r\n\r\n",
        "GET /info RTSP/2.0\r\n\r\n",
        "GET /info RTSP/1.0\r\nNoColonHeader\r\n\r\n",
        "GET /info RTSP/1.0\r\nContent-Length: -1\r\n\r\n",
        "GET /info RTSP/1.0\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx",
        "GET /info RTSP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n",
        "GET /in fo RTSP/1.0\r\n\r\n",
        "GET /info RTSP/1.0\r\nX: a\001b\r\n\r\n",
        "\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        rtsp_request_t req;
        size_t used = 0;
        rtsp_parse_result_t r = rtsp_parse_request((const uint8_t *) bad[i], strlen(bad[i]), &req, &used);
        if (r == RTSP_PARSE_OK) {
            rtsp_request_clear(&req);
        }
        CHECK(r == RTSP_PARSE_BAD || r == RTSP_PARSE_TOO_LARGE);
    }
    /* oversized body */
    const char *huge = "POST /x RTSP/1.0\r\nContent-Length: 999999999\r\n\r\n";
    rtsp_request_t req;
    size_t used = 0;
    CHECK_EQ(rtsp_parse_request((const uint8_t *) huge, strlen(huge), &req, &used), RTSP_PARSE_TOO_LARGE);
    /* endless header without terminator */
    size_t n = RTSP_MAX_HEAD + 10;
    uint8_t *endless = (uint8_t *) malloc(n);
    memset(endless, 'A', n);
    CHECK_EQ(rtsp_parse_request(endless, n, &req, &used), RTSP_PARSE_TOO_LARGE);
    free(endless);
}

TEST(rtsp_photo_uploads_may_be_larger) {
    /* 5 MB is waiting for its body when it is a PUT, and refused for anything else */
    const char *put = "PUT /photo HTTP/1.1\r\nContent-Length: 5000000\r\n\r\n";
    const char *post = "POST /photo HTTP/1.1\r\nContent-Length: 5000000\r\n\r\n";
    const char *too_big = "PUT /photo HTTP/1.1\r\nContent-Length: 16777217\r\n\r\n";
    rtsp_request_t req;
    size_t used = 0;
    CHECK_EQ(rtsp_parse_request((const uint8_t *) put, strlen(put), &req, &used), RTSP_PARSE_INCOMPLETE);
    CHECK_EQ(rtsp_parse_request((const uint8_t *) post, strlen(post), &req, &used), RTSP_PARSE_TOO_LARGE);
    CHECK_EQ(rtsp_parse_request((const uint8_t *) too_big, strlen(too_big), &req, &used), RTSP_PARSE_TOO_LARGE);
}

TEST(rtsp_too_many_headers) {
    char buf[4096];
    int o = snprintf(buf, sizeof(buf), "GET /info RTSP/1.0\r\n");
    for (int i = 0; i < RTSP_MAX_HEADERS + 1; i++) {
        o += snprintf(buf + o, sizeof(buf) - (size_t) o, "X-H%d: v\r\n", i);
    }
    o += snprintf(buf + o, sizeof(buf) - (size_t) o, "\r\n");
    rtsp_request_t req;
    size_t used = 0;
    CHECK_EQ(rtsp_parse_request((const uint8_t *) buf, (size_t) o, &req, &used), RTSP_PARSE_BAD);
}

TEST(rtsp_reply_serialization) {
    rtsp_reply_t reply;
    rtsp_reply_init(&reply);
    rtsp_reply_header(&reply, "CSeq", "3");
    rtsp_reply_header(&reply, "Server", "AirTunes/220.68");
    CHECK(rtsp_reply_body_copy(&reply, "application/octet-stream", "12345", 5));
    uint8_t *out = NULL;
    size_t len = 0;
    CHECK(rtsp_reply_serialize(&reply, "RTSP/1.0", &out, &len) == 0);
    const char *expect = "RTSP/1.0 200 OK\r\nCSeq: 3\r\nServer: AirTunes/220.68\r\n"
                         "Content-Type: application/octet-stream\r\nContent-Length: 5\r\n\r\n12345";
    CHECK_EQ(len, strlen(expect));
    CHECK_MEM(out, expect, len);
    free(out);
    rtsp_reply_clear(&reply);

    rtsp_reply_init(&reply);
    rtsp_reply_status(&reply, 470, "Connection Authorization Required");
    CHECK(rtsp_reply_serialize(&reply, "RTSP/1.0", &out, &len) == 0);
    CHECK_MEM(out, "RTSP/1.0 470 Connection Authorization Required\r\n\r\n", len);
    free(out);

    /* HTTP keeps the connection open, so even an empty answer states its length */
    CHECK(rtsp_reply_serialize(&reply, "HTTP/1.1", &out, &len) == 0);
    CHECK_MEM(out, "HTTP/1.1 470 Connection Authorization Required\r\nContent-Length: 0\r\n\r\n", len);
    free(out);
    rtsp_reply_init(&reply);
    rtsp_reply_status(&reply, 101, "Switching Protocols");
    CHECK(rtsp_reply_serialize(&reply, "HTTP/1.1", &out, &len) == 0);
    CHECK_MEM(out, "HTTP/1.1 101 Switching Protocols\r\n\r\n", len);
    free(out);
}

/* ---- DMAP track info ---- */

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

TEST(dmap_track_info) {
    uint8_t inner[256];
    size_t n = 0;
    const uint8_t mper[8] = { 0, 0, 0, 0, 0, 0, 0, 1 };
    n += dmap_item(inner + n, "mper", mper, sizeof(mper));
    n += dmap_item(inner + n, "minm", "Midnight City", 13);
    n += dmap_item(inner + n, "asar", "M83", 3);
    n += dmap_item(inner + n, "asal", "Hurry Up, We\xe2\x80\x99re Dreaming", 26);
    uint8_t body[300];
    size_t len = dmap_item(body, "mlit", inner, n);
    dmap_track_t t;
    dmap_parse_track(body, len, &t);
    CHECK_STR(t.title, "Midnight City");
    CHECK_STR(t.artist, "M83");
    CHECK_STR(t.album, "Hurry Up, We\xe2\x80\x99re Dreaming");

    /* items outside a listing count as well; missing ones stay empty */
    len = dmap_item(body, "minm", "Intro", 5);
    dmap_parse_track(body, len, &t);
    CHECK_STR(t.title, "Intro");
    CHECK_STR(t.artist, "");
    CHECK_STR(t.album, "");
}

TEST(dmap_rejects_broken_lengths) {
    /* a listing that claims more bytes than there are */
    const uint8_t broken[16] = { 'm', 'l', 'i', 't', 0xff, 0xff, 0xff, 0xf0, 'm', 'i', 'n', 'm', 0, 0, 0, 4 };
    dmap_track_t t;
    dmap_parse_track(broken, sizeof(broken), &t);
    CHECK_STR(t.title, "");
    /* every truncation of a valid body must be safe */
    uint8_t body[64];
    size_t len = dmap_item(body, "minm", "Title", 5);
    len += dmap_item(body + len, "asar", "Artist", 6);
    for (size_t cut = 0; cut <= len; cut++) {
        dmap_parse_track(body, cut, &t);
    }
    dmap_parse_track(NULL, 0, &t);
    CHECK_STR(t.title, "");
}

TEST(dmap_long_text_is_cut_on_a_character_boundary) {
    static uint8_t text[1200];
    memset(text, 'x', sizeof(text));
    text[254] = 0xc3; /* a two-byte sequence across the 255-byte limit */
    text[255] = 0xa9;
    static uint8_t body[1300];
    size_t len = dmap_item(body, "minm", text, sizeof(text));
    dmap_track_t t;
    dmap_parse_track(body, len, &t);
    CHECK_EQ(strlen(t.title), 254);
    /* invalid UTF-8 and control characters do not get through */
    len = dmap_item(body, "asar", "A\xff\nB", 4);
    dmap_parse_track(body, len, &t);
    CHECK_STR(t.artist, "A??B");
}

/* ---- utilities ---- */

TEST(util_encoding) {
    char hex[9];
    uint8_t in[4] = { 0xde, 0xad, 0x01, 0xff };
    hex_encode(in, 4, hex);
    CHECK_STR(hex, "dead01ff");
    uint8_t back[4];
    CHECK(hex_decode("DEAD01ff", back, 4) == 0 && memcmp(back, in, 4) == 0);
    CHECK(hex_decode("dead01f", back, 4) != 0);
    CHECK(hex_decode("zz", back, 1) != 0);

    char b64[32];
    base64_encode((const uint8_t *) "any carnal pleas", 16, b64);
    CHECK_STR(b64, "YW55IGNhcm5hbCBwbGVhcw==");
    base64_encode((const uint8_t *) "ab", 2, b64);
    CHECK_STR(b64, "YWI=");

    char name[64];
    sanitize_utf8(name, sizeof(name), "ok\x01\xff" "Ж", 6);
    CHECK_STR(name, "ok??Ж");
    CHECK_EQ(parse_uint("123", 3, 1000), 123);
    CHECK_EQ(parse_uint("1234", 4, 1000), -1);
    CHECK_EQ(parse_uint("12a", 3, 1000), -1);
    CHECK_EQ(parse_uint("", 0, 1000), -1);
}
