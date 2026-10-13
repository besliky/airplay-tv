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
 * Streams a generated test picture (and optionally a tone) to a running
 * receiver, e.g. the app on an emulator or a TV:
 *
 *   airplaytv_sender <ip> [port] [seconds] [--pair] [--audio] [--portrait]
 *   airplaytv_sender <ip> [port] [seconds] [--pair] --music
 *
 * --music plays a tone like a music app instead: no picture, but track info, a
 * cover and the position, a pause in the middle and a second track after it.
 * --portrait sends an upright picture, as from an iPhone held upright.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fake_sender.h"
#include "h264_gen.h"
#include "ntp.h"
#include "util.h"

/* A 64x64 PNG with a warm vertical gradient, as the cover of the first track. */
static const uint8_t kCover[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x08, 0x02, 0x00, 0x00, 0x00, 0x25, 0x0b, 0xe6,
    0x89, 0x00, 0x00, 0x01, 0x41, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0xd5, 0xc2, 0x89, 0x2e, 0x02,
    0x00, 0x00, 0xc7, 0xe1, 0xff, 0xd3, 0x61, 0x0c, 0x63, 0x18, 0x2b, 0x72, 0x96, 0xca, 0x51, 0x42,
    0xa9, 0x44, 0x45, 0x94, 0xfb, 0x8a, 0x31, 0x5e, 0x05, 0xe5, 0x48, 0xca, 0x2d, 0x52, 0x72, 0xa4,
    0x9e, 0xc3, 0x73, 0xfc, 0xbe, 0x7d, 0xaa, 0xd6, 0xc4, 0xd1, 0x55, 0xad, 0x8d, 0xa3, 0xab, 0x52,
    0xb7, 0x87, 0xae, 0x4a, 0xfd, 0x2e, 0xba, 0xfe, 0x1a, 0x76, 0xd0, 0x55, 0x6e, 0xdc, 0x46, 0x57,
    0xb9, 0x69, 0x0b, 0x5d, 0xbf, 0xcd, 0x9b, 0xe8, 0xfa, 0x69, 0xd9, 0x40, 0xd7, 0x4f, 0xeb, 0x3a,
    0xba, 0xbe, 0xdb, 0xd6, 0xd0, 0xf5, 0xdd, 0xbe, 0x8a, 0xae, 0xaf, 0x8e, 0x15, 0x74, 0x95, 0x3a,
    0x63, 0xe8, 0x2a, 0x75, 0x45, 0xd1, 0xf5, 0x69, 0x58, 0x46, 0x57, 0xd1, 0xb8, 0x84, 0xae, 0x62,
    0xf7, 0x12, 0xba, 0x0a, 0xa6, 0x08, 0xba, 0x0a, 0xbd, 0x8b, 0xe8, 0xfa, 0xe8, 0x5f, 0x40, 0x57,
    0x7e, 0x20, 0x8c, 0xae, 0xfc, 0x60, 0x08, 0x5d, 0xef, 0xe6, 0x20, 0xba, 0xde, 0x2c, 0xf3, 0xe8,
    0x7a, 0xb3, 0xce, 0xa1, 0x2b, 0x67, 0x0b, 0xa0, 0x2b, 0x67, 0x9f, 0x45, 0xd7, 0xeb, 0xa8, 0x1f,
    0x5d, 0x2f, 0x63, 0x3e, 0x74, 0xbd, 0x38, 0xbc, 0xe8, 0x7a, 0x76, 0xce, 0xa0, 0xeb, 0xc9, 0xe5,
    0x41, 0xd7, 0xd3, 0xa4, 0x07, 0x5d, 0x8f, 0x53, 0x6e, 0x74, 0x3d, 0xba, 0xa7, 0xd1, 0xf5, 0xe0,
    0x99, 0x42, 0xd7, 0xbd, 0x77, 0x12, 0x5d, 0xf7, 0x3e, 0x17, 0xba, 0xee, 0xfc, 0x13, 0xe8, 0xca,
    0x06, 0x9c, 0xe8, 0xca, 0xce, 0x39, 0xd0, 0x95, 0x09, 0x8e, 0xa3, 0x2b, 0x13, 0x1a, 0x43, 0xd7,
    0x6d, 0x78, 0x14, 0x5d, 0xe9, 0xc5, 0x11, 0x74, 0xa5, 0x23, 0x76, 0x74, 0xdd, 0x2c, 0xdb, 0xd0,
    0x95, 0x8a, 0x5a, 0xd1, 0x95, 0x8a, 0x59, 0xd1, 0x75, 0xbd, 0x3a, 0x8c, 0xae, 0xeb, 0x35, 0x0b,
    0xba, 0xae, 0x36, 0xcc, 0xe8, 0xba, 0xdc, 0x1c, 0x42, 0xd7, 0xe5, 0xd6, 0x20, 0xba, 0x2e, 0x76,
    0x06, 0xd0, 0x95, 0xdc, 0xed, 0x47, 0x57, 0x32, 0xde, 0x87, 0xae, 0xc4, 0x7e, 0x2f, 0xba, 0x12,
    0x07, 0x26, 0x74, 0x9d, 0x1f, 0xf6, 0xa0, 0xeb, 0xec, 0xa8, 0x1b, 0x5d, 0x67, 0xc7, 0x46, 0x74,
    0x9d, 0x9e, 0x18, 0xd0, 0xff, 0x01, 0x7b, 0x66, 0xd9, 0x96, 0x3b, 0x1a, 0x0e, 0xc7, 0x00, 0x00,
    0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

#define SAMPLE_RATE 44100

static void sleep_until(uint64_t deadline_ns) {
    uint64_t now = time_mono_ns();
    if (deadline_ns > now) {
        uint64_t d = deadline_ns - now;
        struct timespec ts = { (time_t) (d / NS_PER_SEC), (long) (d % NS_PER_SEC) };
        nanosleep(&ts, NULL);
    }
}

/* Music: 40% playing, 15% paused (no audio, as senders pause), then a second track without a cover. */
static int play_music(fake_sender_t *f, int seconds) {
    const uint32_t start_ts = 1000;
    const uint32_t length = 200 * SAMPLE_RATE;
    fs_set_track(f, "Midnight on the Harbour", "The Paper Lanterns", "Slow Tide");
    fs_set_parameter(f, "image/png", kCover, sizeof(kCover));
    fs_set_progress(f, start_ts, start_ts + 30 * SAMPLE_RATE, start_ts + length);
    fs_set_volume(f, -6.0f);

    uint8_t alac[2048];
    uint64_t start = time_mono_ns();
    uint64_t total = (uint64_t) seconds * NS_PER_SEC;
    uint64_t next_audio = start;
    uint64_t next_feedback = start;
    int packet = 0;
    bool second_track = false;
    while (time_mono_ns() - start < total) {
        uint64_t now = time_mono_ns();
        uint64_t elapsed = now - start;
        bool paused = elapsed > total * 40 / 100 && elapsed < total * 55 / 100;
        if (!second_track && elapsed >= total * 60 / 100) {
            second_track = true;
            fs_set_track(f, "Northern Lights", "Aurora Fields", "Polar Nights");
            fs_set_progress(f, start_ts, start_ts, start_ts + length);
        }
        if (now >= next_audio) {
            if (!paused) {
                if (packet % 100 == 0) {
                    fs_send_sync(f);
                }
                size_t n = fs_make_alac_frame(alac, sizeof(alac), packet++);
                if (fs_send_audio(f, alac, n, 352) != 0) {
                    break;
                }
            }
            next_audio += 352ULL * NS_PER_SEC / SAMPLE_RATE;
        }
        if (now >= next_feedback) {
            fs_feedback(f);
            next_feedback += 2 * NS_PER_SEC;
        }
        sleep_until(next_audio);
    }
    printf("sent %d audio packets\n", packet);
    return 0;
}

static int play_mirror(fake_sender_t *f, int seconds, bool audio, bool portrait) {
    if (fs_setup_mirror(f, 0x7e57ULL) != 0) {
        fprintf(stderr, "mirror SETUP failed\n");
        return 1;
    }
    if (audio && fs_setup_audio(f, 2, 352) != 0) {
        fprintf(stderr, "audio SETUP failed\n");
        return 1;
    }

    const int width = portrait ? 352 : 640;
    const int height = portrait ? 640 : 352;
    const int fps = 30;
    h264_gen_t g;
    h264_gen_init(&g, width, height);
    uint8_t avcc[128];
    size_t avcc_len = h264_gen_avcc(&g, avcc, sizeof(avcc));
    fs_send_codec(f, avcc, avcc_len, width, height, 0x16);

    static uint8_t au[1024 * 1024];
    uint8_t alac[2048];
    uint64_t start = time_mono_ns();
    uint64_t next_video = start;
    uint64_t next_audio = start;
    uint64_t next_feedback = start;
    int frame = 0;
    int packet = 0;
    while (time_mono_ns() - start < (uint64_t) seconds * NS_PER_SEC) {
        uint64_t now = time_mono_ns();
        if (now >= next_video) {
            size_t n = h264_gen_frame(&g, frame % fps == 0, frame / 2, au, sizeof(au));
            if (fs_send_video(f, au, n, ns_to_ntp(now)) != 0) {
                fprintf(stderr, "video stream closed\n");
                break;
            }
            frame++;
            next_video += NS_PER_SEC / fps;
        }
        if (audio && now >= next_audio) {
            if (packet % 100 == 0) {
                fs_send_sync(f);
            }
            size_t n = fs_make_alac_frame(alac, sizeof(alac), packet++);
            fs_send_audio(f, alac, n, 352);
            next_audio += 352ULL * NS_PER_SEC / SAMPLE_RATE;
        }
        if (now >= next_feedback) {
            fs_feedback(f);
            next_feedback += 2 * NS_PER_SEC;
        }
        uint64_t next = next_video;
        if (audio && next_audio < next) next = next_audio;
        sleep_until(next);
    }
    printf("sent %d frames%s\n", frame, audio ? " with audio" : "");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <ip> [port] [seconds] [--pair] [--audio | --music] [--portrait]\n", argv[0]);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    const char *ip = argv[1];
    int port = argc > 2 ? atoi(argv[2]) : 7000;
    int seconds = argc > 3 ? atoi(argv[3]) : 10;
    bool pair = false;
    bool audio = false;
    bool music = false;
    bool pin_only = false;
    bool portrait = false;
    for (int i = 2; i < argc; i++) {
        pair |= strcmp(argv[i], "--pair") == 0;
        audio |= strcmp(argv[i], "--audio") == 0;
        music |= strcmp(argv[i], "--music") == 0;
        pin_only |= strcmp(argv[i], "--pin-screen") == 0;
        portrait |= strcmp(argv[i], "--portrait") == 0;
    }

    fake_sender_t f;
    if (fs_connect(&f, ip, (uint16_t) port) != 0) {
        fprintf(stderr, "cannot connect to %s:%d\n", ip, port);
        return 1;
    }
    if (pin_only) {
        /* ask a PIN-protected receiver to show its code, then wait */
        int status = 0;
        fs_request(&f, "POST", "/pair-pin-start", NULL, NULL, 0, &status, NULL, NULL);
        printf("pair-pin-start: %d\n", status);
        sleep_until(time_mono_ns() + (uint64_t) seconds * NS_PER_SEC);
        fs_close(&f);
        return status == 200 ? 0 : 1;
    }
    if (pair ? fs_pair(&f) != 0 : fs_info(&f, NULL) != 0) {
        fprintf(stderr, "pairing/info failed\n");
        return 1;
    }
    if (fs_fairplay(&f) != 0) {
        fprintf(stderr, "fp-setup failed\n");
        return 1;
    }
    int st = fs_setup_session(&f, "Test Sender", "iPhone15,2");
    if (st != 200) {
        fprintf(stderr, "SETUP failed (%d)\n", st);
        return 1;
    }
    int result;
    if (music) {
        result = fs_setup_audio(&f, 2, 352) == 0 ? play_music(&f, seconds) : 1;
    } else {
        result = play_mirror(&f, seconds, audio, portrait);
    }
    fs_teardown(&f, 0);
    fs_close(&f);
    return result;
}
