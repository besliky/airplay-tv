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
 * Reader for the DMAP track description a sender pushes with SET_PARAMETER
 * (application/x-dmap-tagged). DMAP items are a four-character tag, a
 * big-endian 32-bit length and the payload; containers such as "mlit" hold
 * further items.
 */

#ifndef AIRPLAYTV_DMAP_H
#define AIRPLAYTV_DMAP_H

#include "common.h"

#define DMAP_TEXT_MAX 256

typedef struct {
    char title[DMAP_TEXT_MAX];   /* "minm" */
    char artist[DMAP_TEXT_MAX];  /* "asar" */
    char album[DMAP_TEXT_MAX];   /* "asal" */
} dmap_track_t;

/* Fills out with the title, artist and album found in data. Text is valid UTF-8, cut at a
 * character boundary when it is too long; missing fields stay empty. Items whose length runs
 * past the end of their container end the scan. */
void dmap_parse_track(const uint8_t *data, size_t len, dmap_track_t *out);

#endif
