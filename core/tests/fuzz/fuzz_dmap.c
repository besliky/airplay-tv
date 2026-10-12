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

#include "dmap.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    dmap_track_t track;
    dmap_parse_track(data, size, &track);
    /* every field must be terminated within its buffer */
    if (memchr(track.title, 0, sizeof(track.title)) == NULL || memchr(track.artist, 0, sizeof(track.artist)) == NULL ||
        memchr(track.album, 0, sizeof(track.album)) == NULL) {
        abort();
    }
    return 0;
}
