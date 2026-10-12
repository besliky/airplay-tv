/*
 * AirPlay TV - AirPlay screen mirroring receiver for Android TV
 * Copyright (C) 2026 besliky
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "dmap.h"

#include "util.h"

#define DMAP_MAX_DEPTH 4

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

static void scan(const uint8_t *data, size_t len, int depth, dmap_track_t *out) {
    size_t pos = 0;
    while (len - pos >= 8) {
        const uint8_t *tag = data + pos;
        size_t item = be32(data + pos + 4);
        pos += 8;
        if (item > len - pos) {
            return;
        }
        const char *text = (const char *) data + pos;
        if (memcmp(tag, "minm", 4) == 0) {
            sanitize_utf8(out->title, sizeof(out->title), text, item);
        } else if (memcmp(tag, "asar", 4) == 0) {
            sanitize_utf8(out->artist, sizeof(out->artist), text, item);
        } else if (memcmp(tag, "asal", 4) == 0) {
            sanitize_utf8(out->album, sizeof(out->album), text, item);
        } else if (memcmp(tag, "mlit", 4) == 0 && depth < DMAP_MAX_DEPTH) {
            scan(data + pos, item, depth + 1, out);
        }
        pos += item;
    }
}

void dmap_parse_track(const uint8_t *data, size_t len, dmap_track_t *out) {
    memset(out, 0, sizeof(*out));
    if (data) {
        scan(data, len, 0, out);
    }
}
