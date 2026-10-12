/*
 * AirPlay TV - AirPlay screen mirroring receiver for Android TV
 * Copyright (C) 2026 besliky
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "fake_sender.h"

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bplist.h"
#include "mbedtls/aes.h"
#include "mbedtls/bignum.h"
#include "mbedtls/sha1.h"
#include "mirror.h"
#include "ntp.h"
#include "playfair.h"
#include "util.h"

#define TIMEOUT_MS 5000

static int wait_readable(int fd, int timeout_ms) {
    struct pollfd p = { .fd = fd, .events = POLLIN };
    return poll(&p, 1, timeout_ms) > 0 ? 0 : -1;
}

static int send_all(int fd, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *) data;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += n;
        len -= (size_t) n;
    }
    return 0;
}

static int udp_socket(uint16_t *port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(a);
    if (fd < 0 || bind(fd, (struct sockaddr *) &a, sizeof(a)) != 0 ||
        getsockname(fd, (struct sockaddr *) &a, &len) != 0) {
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }
    *port = ntohs(a.sin_port);
    return fd;
}

int fs_connect(fake_sender_t *f, const char *ip, uint16_t port) {
    memset(f, 0, sizeof(*f));
    f->fd = f->mirror_fd = f->audio_fd = f->control_fd = f->timing_fd = -1;
    snprintf(f->user_agent, sizeof(f->user_agent), "AirPlay/860.7.1");
    f->server.sin_family = AF_INET;
    f->server.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &f->server.sin_addr) != 1) {
        return -1;
    }
    f->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (f->fd < 0 || connect(f->fd, (struct sockaddr *) &f->server, sizeof(f->server)) != 0) {
        return -1;
    }
    int one = 1;
    setsockopt(f->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    uint8_t seed[32];
    crypto_random(seed, sizeof(seed));
    ed25519_from_seed(seed, f->ed_secret, f->ed_public);
    f->timing_fd = udp_socket(&f->timing_port);
    return f->timing_fd >= 0 ? 0 : -1;
}

void fs_close(fake_sender_t *f) {
    int *fds[] = { &f->fd, &f->mirror_fd, &f->audio_fd, &f->control_fd, &f->timing_fd };
    for (size_t i = 0; i < sizeof(fds) / sizeof(fds[0]); i++) {
        if (*fds[i] >= 0) {
            close(*fds[i]);
            *fds[i] = -1;
        }
    }
    if (f->ctr_ready) {
        aes_ctr_free(&f->ctr);
        f->ctr_ready = false;
    }
}

/* Reads one reply (RTSP/1.0 or HTTP/1.1). The head is copied to head when it is not NULL;
 * *body is malloc'd and may be NULL. */
static int read_reply(fake_sender_t *f, int *status, char *head, size_t head_cap, uint8_t **body,
                      size_t *body_len) {
    char buf[4096];
    size_t got = 0;
    char *end = NULL;
    while (!end) {
        if (got + 1 >= sizeof(buf) || wait_readable(f->fd, TIMEOUT_MS) != 0) {
            return -1;
        }
        ssize_t r = recv(f->fd, buf + got, sizeof(buf) - 1 - got, 0);
        if (r <= 0) {
            return -1;
        }
        got += (size_t) r;
        buf[got] = 0;
        end = strstr(buf, "\r\n\r\n");
    }
    size_t head_len = (size_t) (end - buf) + 4;
    int code = 0;
    if (sscanf(buf, "RTSP/1.0 %d", &code) != 1 && sscanf(buf, "HTTP/1.1 %d", &code) != 1) {
        return -1;
    }
    if (head && head_cap) {
        size_t n = head_len < head_cap ? head_len : head_cap - 1;
        memcpy(head, buf, n);
        head[n] = 0;
    }
    size_t content_length = 0;
    char *cl = strstr(buf, "Content-Length: ");
    if (cl && cl < end) {
        content_length = (size_t) strtoul(cl + 16, NULL, 10);
    }
    uint8_t *out = content_length ? (uint8_t *) malloc(content_length) : NULL;
    if (content_length && !out) {
        return -1;
    }
    size_t have = got - head_len;
    if (have > content_length) {
        have = content_length;
    }
    if (have) {
        memcpy(out, buf + head_len, have);
    }
    while (have < content_length) {
        if (wait_readable(f->fd, TIMEOUT_MS) != 0) {
            free(out);
            return -1;
        }
        ssize_t r = recv(f->fd, out + have, content_length - have, 0);
        if (r <= 0) {
            free(out);
            return -1;
        }
        have += (size_t) r;
    }
    if (status) {
        *status = code;
    }
    if (body) {
        *body = out;
        *body_len = content_length;
    } else {
        free(out);
    }
    return 0;
}

int fs_request(fake_sender_t *f, const char *method, const char *url, const char *content_type,
               const void *data, size_t len, int *status, uint8_t **body, size_t *body_len) {
    char head[768];
    int n = snprintf(head, sizeof(head), "%s %s RTSP/1.0\r\nCSeq: %d\r\nUser-Agent: %s\r\n%s", method, url,
                     ++f->cseq, f->user_agent, f->extra_headers);
    if (content_type) {
        n += snprintf(head + n, sizeof(head) - (size_t) n, "Content-Type: %s\r\n", content_type);
    }
    n += snprintf(head + n, sizeof(head) - (size_t) n, "Content-Length: %zu\r\n\r\n", len);
    if (send_all(f->fd, head, (size_t) n) != 0 || (len && send_all(f->fd, data, len) != 0)) {
        return -1;
    }
    return read_reply(f, status, NULL, 0, body, body_len);
}

int fs_http(fake_sender_t *f, const char *method, const char *url, const char *headers, const void *data,
            size_t len, char *reply_head, size_t head_cap, uint8_t **body, size_t *body_len) {
    char head[1024];
    int n = snprintf(head, sizeof(head), "%s %s HTTP/1.1\r\nUser-Agent: MediaControl/1.0\r\n%sContent-Length: %zu\r\n\r\n",
                     method, url, headers ? headers : "", len);
    if (n <= 0 || (size_t) n >= sizeof(head) || send_all(f->fd, head, (size_t) n) != 0 ||
        (len && send_all(f->fd, data, len) != 0)) {
        return -1;
    }
    int status = 0;
    return read_reply(f, &status, reply_head, head_cap, body, body_len) == 0 ? status : -1;
}

static int request_plist(fake_sender_t *f, const char *method, const char *url, bp_node_t *req,
                         int *status, bp_node_t **resp) {
    uint8_t *data = NULL;
    size_t len = 0;
    if (req && bp_write(req, &data, &len) != 0) {
        bp_free(req);
        return -1;
    }
    bp_free(req);
    uint8_t *body = NULL;
    size_t body_len = 0;
    int r = fs_request(f, method, url, req ? "application/x-apple-binary-plist" : NULL, data, len, status,
                       &body, &body_len);
    free(data);
    if (r == 0 && resp) {
        *resp = body ? bp_parse(body, body_len) : NULL;
    }
    free(body);
    return r;
}

int fs_info(fake_sender_t *f, uint8_t server_pk[32]) {
    bp_node_t *resp = NULL;
    int status = 0;
    if (request_plist(f, "GET", "/info", NULL, &status, &resp) != 0 || status != 200 || !resp) {
        bp_free(resp);
        return -1;
    }
    size_t len = 0;
    const uint8_t *pk = bp_get_data(bp_dict_get(resp, "pk"), &len);
    int ok = pk && len == 32;
    if (ok && server_pk) {
        memcpy(server_pk, pk, 32);
    }
    bp_free(resp);
    return ok ? 0 : -1;
}

static void verify_keys(const uint8_t shared[32], uint8_t key[16], uint8_t iv[16]) {
    uint8_t h[64];
    sha512_2("Pair-Verify-AES-Key", 19, shared, 32, h);
    memcpy(key, h, 16);
    sha512_2("Pair-Verify-AES-IV", 18, shared, 32, h);
    memcpy(iv, h, 16);
}

static int pair_verify(fake_sender_t *f) {
    uint8_t x_secret[32];
    uint8_t x_public[32];
    x25519_generate(x_secret, x_public);
    uint8_t msg1[4 + 64] = { 1, 0, 0, 0 };
    memcpy(msg1 + 4, x_public, 32);
    memcpy(msg1 + 36, f->ed_public, 32);
    uint8_t *resp = NULL;
    size_t resp_len = 0;
    int status = 0;
    if (fs_request(f, "POST", "/pair-verify", "application/octet-stream", msg1, sizeof(msg1), &status, &resp,
                   &resp_len) != 0 || status != 200 || resp_len != 96) {
        free(resp);
        return -1;
    }
    uint8_t server_x[32];
    memcpy(server_x, resp, 32);
    if (x25519_shared(f->shared, x_secret, server_x) != 0) {
        free(resp);
        return -1;
    }
    uint8_t key[16];
    uint8_t iv[16];
    verify_keys(f->shared, key, iv);
    aes_ctr_t ctr;
    aes_ctr_init(&ctr, key, iv);
    uint8_t sig[64];
    aes_ctr_xcrypt(&ctr, resp + 32, sig, 64);
    free(resp);

    /* the server signed (server_x | client_x) */
    uint8_t signed_msg[64];
    memcpy(signed_msg, server_x, 32);
    memcpy(signed_msg + 32, x_public, 32);
    if (ed25519_verify(sig, f->server_ed, signed_msg, 64) != 0) {
        aes_ctr_free(&ctr);
        return -2;
    }
    /* our signature over (client_x | server_x), encrypted with the continuing keystream */
    uint8_t mine[64];
    memcpy(signed_msg, x_public, 32);
    memcpy(signed_msg + 32, server_x, 32);
    ed25519_sign(mine, f->ed_secret, signed_msg, 64);
    uint8_t msg2[4 + 64] = { 0, 0, 0, 0 };
    aes_ctr_xcrypt(&ctr, mine, msg2 + 4, 64);
    aes_ctr_free(&ctr);
    if (fs_request(f, "POST", "/pair-verify", "application/octet-stream", msg2, sizeof(msg2), &status, NULL,
                   NULL) != 0 || status != 200) {
        return -3;
    }
    f->have_shared = true;
    return 0;
}

int fs_pair(fake_sender_t *f) {
    if (fs_info(f, f->server_ed) != 0) {
        return -1;
    }
    uint8_t *resp = NULL;
    size_t resp_len = 0;
    int status = 0;
    if (fs_request(f, "POST", "/pair-setup", "application/octet-stream", f->ed_public, 32, &status, &resp,
                   &resp_len) != 0 || status != 200 || resp_len != 32 || memcmp(resp, f->server_ed, 32) != 0) {
        free(resp);
        return -1;
    }
    free(resp);
    return pair_verify(f);
}

/* ---- SRP-6a client (for PIN pairing tests) ---- */

static const char kN[] =
    "AC6BDB41324A9A9BF166DE5E1389582FAF72B6651987EE07FC3192943DB56050A37329CBB4"
    "A099ED8193E0757767A13DD52312AB4B03310DCD7F48A9DA04FD50E8083969EDB767B0CF60"
    "95179A163AB3661A05FBD5FAAAE82918A9962F0B93B855F97993EC975EEAA80D740ADBF4FF"
    "747359D041D5C33EA71D281E446B14773BCA97B43A23FB801676BD207A436C6481F1D2B907"
    "8717461A5B9D32E688F87748544523B524B0D57D5EA77A2775D2ECFA032CFBDBF52FB37861"
    "60279004E57AE6AF874E7303CE53299CCC041C7BC308D82A5698F3A8D0C38271AE35F8E9DB"
    "FBB694B5C803D89F7AE435DE236D525F54759B65E372FCD68EF20FA7111F9E4AFF73";

static void mpi_min_bytes(const mbedtls_mpi *x, uint8_t *buf, size_t *len) {
    *len = mbedtls_mpi_size(x);
    mbedtls_mpi_write_binary(x, buf, *len);
}

static void sha1_update_mpi(mbedtls_sha1_context *c, const mbedtls_mpi *x) {
    uint8_t buf[256];
    size_t len;
    mpi_min_bytes(x, buf, &len);
    mbedtls_sha1_update(c, buf, len);
}

static void hash_pad(const mbedtls_mpi *a, const mbedtls_mpi *b, mbedtls_mpi *out) {
    uint8_t buf[512];
    uint8_t d[20];
    mbedtls_mpi_write_binary(a, buf, 256);
    mbedtls_mpi_write_binary(b, buf + 256, 256);
    mbedtls_sha1(buf, 512, d);
    mbedtls_mpi_read_binary(out, d, 20);
}

int fs_pair_pin(fake_sender_t *f, const char *(*pin_source)(void *ctx), void *ctx) {
    const char *user = "AA:BB:CC:DD:EE:FF";
    int status = 0;
    if (fs_info(f, f->server_ed) != 0) {
        return -1;
    }
    if (fs_request(f, "POST", "/pair-pin-start", NULL, NULL, 0, &status, NULL, NULL) != 0 || status != 200) {
        return -1;
    }
    const char *pin = pin_source(ctx);
    if (!pin) {
        return -1;
    }

    bp_node_t *req = bp_new_dict();
    bp_dict_set(req, "method", bp_new_string("pin"));
    bp_dict_set(req, "user", bp_new_string(user));
    bp_node_t *resp = NULL;
    if (request_plist(f, "POST", "/pair-setup-pin", req, &status, &resp) != 0 || status != 200 || !resp) {
        bp_free(resp);
        return -2;
    }
    size_t salt_len = 0;
    size_t B_len = 0;
    const uint8_t *salt = bp_get_data(bp_dict_get(resp, "salt"), &salt_len);
    const uint8_t *B_bytes = bp_get_data(bp_dict_get(resp, "pk"), &B_len);
    if (!salt || !B_bytes || salt_len != 16) {
        bp_free(resp);
        return -2;
    }

    mbedtls_mpi N, g, B, a, A, x, k, u, S, t1, t2, e;
    mbedtls_mpi *all[] = { &N, &g, &B, &a, &A, &x, &k, &u, &S, &t1, &t2, &e };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        mbedtls_mpi_init(all[i]);
    }
    mbedtls_mpi_read_string(&N, 16, kN);
    mbedtls_mpi_lset(&g, 2);
    mbedtls_mpi_read_binary(&B, B_bytes, B_len);
    uint8_t a_bytes[32];
    crypto_random(a_bytes, sizeof(a_bytes));
    mbedtls_mpi_read_binary(&a, a_bytes, sizeof(a_bytes));
    mbedtls_mpi_exp_mod(&A, &g, &a, &N, NULL);

    /* x = H(s | H(I:P)) */
    uint8_t inner[20];
    uint8_t xd[20];
    mbedtls_sha1_context c;
    mbedtls_sha1_init(&c);
    mbedtls_sha1_starts(&c);
    mbedtls_sha1_update(&c, (const uint8_t *) user, strlen(user));
    mbedtls_sha1_update(&c, (const uint8_t *) ":", 1);
    mbedtls_sha1_update(&c, (const uint8_t *) pin, strlen(pin));
    mbedtls_sha1_finish(&c, inner);
    mbedtls_sha1_starts(&c);
    mbedtls_sha1_update(&c, salt, 16);
    mbedtls_sha1_update(&c, inner, 20);
    mbedtls_sha1_finish(&c, xd);
    mbedtls_mpi_read_binary(&x, xd, 20);

    /* S = (B - k * g^x) ^ (a + u * x) mod N */
    hash_pad(&N, &g, &k);
    hash_pad(&A, &B, &u);
    mbedtls_mpi_exp_mod(&t1, &g, &x, &N, NULL);
    mbedtls_mpi_mul_mpi(&t1, &t1, &k);
    mbedtls_mpi_mod_mpi(&t1, &t1, &N);
    mbedtls_mpi_sub_mpi(&t2, &B, &t1);
    mbedtls_mpi_mod_mpi(&t2, &t2, &N);
    mbedtls_mpi_mul_mpi(&e, &u, &x);
    mbedtls_mpi_add_mpi(&e, &e, &a);
    mbedtls_mpi_exp_mod(&S, &t2, &e, &N, NULL);

    uint8_t s_bytes[256];
    size_t s_len;
    mpi_min_bytes(&S, s_bytes, &s_len);
    uint8_t K[40];
    static const uint8_t c0[4] = { 0, 0, 0, 0 };
    static const uint8_t c1[4] = { 0, 0, 0, 1 };
    mbedtls_sha1_starts(&c);
    mbedtls_sha1_update(&c, s_bytes, s_len);
    mbedtls_sha1_update(&c, c0, 4);
    mbedtls_sha1_finish(&c, K);
    mbedtls_sha1_starts(&c);
    mbedtls_sha1_update(&c, s_bytes, s_len);
    mbedtls_sha1_update(&c, c1, 4);
    mbedtls_sha1_finish(&c, K + 20);

    uint8_t hn[20];
    uint8_t hg[20];
    uint8_t hi[20];
    uint8_t M1[20];
    mbedtls_sha1_starts(&c);
    sha1_update_mpi(&c, &N);
    mbedtls_sha1_finish(&c, hn);
    mbedtls_sha1_starts(&c);
    sha1_update_mpi(&c, &g);
    mbedtls_sha1_finish(&c, hg);
    mbedtls_sha1((const uint8_t *) user, strlen(user), hi);
    for (int i = 0; i < 20; i++) {
        hn[i] ^= hg[i];
    }
    mbedtls_sha1_starts(&c);
    mbedtls_sha1_update(&c, hn, 20);
    mbedtls_sha1_update(&c, hi, 20);
    mbedtls_sha1_update(&c, salt, 16);
    sha1_update_mpi(&c, &A);
    sha1_update_mpi(&c, &B);
    mbedtls_sha1_update(&c, K, 40);
    mbedtls_sha1_finish(&c, M1);
    uint8_t expected_M2[20];
    mbedtls_sha1_starts(&c);
    sha1_update_mpi(&c, &A);
    mbedtls_sha1_update(&c, M1, 20);
    mbedtls_sha1_update(&c, K, 40);
    mbedtls_sha1_finish(&c, expected_M2);
    mbedtls_sha1_free(&c);

    uint8_t A_bytes[256];
    size_t A_len;
    mpi_min_bytes(&A, A_bytes, &A_len);
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        mbedtls_mpi_free(all[i]);
    }
    bp_free(resp);

    req = bp_new_dict();
    bp_dict_set(req, "pk", bp_new_data(A_bytes, A_len));
    bp_dict_set(req, "proof", bp_new_data(M1, 20));
    resp = NULL;
    if (request_plist(f, "POST", "/pair-setup-pin", req, &status, &resp) != 0 || status != 200 || !resp) {
        bp_free(resp);
        return -3;
    }
    size_t m2_len = 0;
    const uint8_t *M2 = bp_get_data(bp_dict_get(resp, "proof"), &m2_len);
    int ok = M2 && m2_len == 20 && memcmp(M2, expected_M2, 20) == 0;
    bp_free(resp);
    if (!ok) {
        return -4;
    }

    /* step 3: exchange Ed25519 keys under AES-GCM keyed from K */
    uint8_t h[64];
    uint8_t key[16];
    uint8_t iv[16];
    sha512_2("Pair-Setup-AES-Key", 18, K, 40, h);
    memcpy(key, h, 16);
    sha512_2("Pair-Setup-AES-IV", 17, K, 40, h);
    memcpy(iv, h, 16);
    iv[15]++;
    uint8_t epk[32];
    uint8_t tag[16];
    aes_gcm_encrypt(key, iv, f->ed_public, 32, epk, tag);
    req = bp_new_dict();
    bp_dict_set(req, "epk", bp_new_data(epk, 32));
    bp_dict_set(req, "authTag", bp_new_data(tag, 16));
    resp = NULL;
    if (request_plist(f, "POST", "/pair-setup-pin", req, &status, &resp) != 0 || status != 200 || !resp) {
        bp_free(resp);
        return -5;
    }
    size_t sepk_len = 0;
    size_t stag_len = 0;
    const uint8_t *sepk = bp_get_data(bp_dict_get(resp, "epk"), &sepk_len);
    const uint8_t *stag = bp_get_data(bp_dict_get(resp, "authTag"), &stag_len);
    iv[15]++;
    uint8_t server_pk[32];
    ok = sepk && stag && sepk_len == 32 && stag_len == 16 &&
         aes_gcm_decrypt(key, iv, sepk, 32, server_pk, stag) == 0 && memcmp(server_pk, f->server_ed, 32) == 0;
    bp_free(resp);
    if (!ok) {
        return -6;
    }
    return pair_verify(f);
}

int fs_fairplay(fake_sender_t *f) {
    uint8_t setup[16] = { 'F', 'P', 'L', 'Y', 0x03, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x02, 0x00, 0x02, 0xbb };
    uint8_t *resp = NULL;
    size_t resp_len = 0;
    int status = 0;
    if (fs_request(f, "POST", "/fp-setup", "application/octet-stream", setup, sizeof(setup), &status, &resp,
                   &resp_len) != 0 || status != 200 || resp_len != 142 || memcmp(resp, "FPLY", 4) != 0) {
        free(resp);
        return -1;
    }
    free(resp);
    uint8_t *msg = f->keymsg;
    crypto_random(msg, 164);
    memcpy(msg, "FPLY", 4);
    msg[4] = 0x03;
    msg[5] = 0x01;
    msg[6] = 0x03;
    msg[7] = msg[8] = msg[9] = msg[10] = 0;
    msg[11] = 0x98;
    msg[12] = 0x02; /* mode */
    if (fs_request(f, "POST", "/fp-setup", "application/octet-stream", msg, 164, &status, &resp, &resp_len) != 0 ||
        status != 200 || resp_len != 32 || memcmp(resp + 12, msg + 144, 20) != 0) {
        free(resp);
        return -1;
    }
    free(resp);
    return 0;
}

int fs_setup_session(fake_sender_t *f, const char *name, const char *model) {
    uint8_t ekey[72];
    crypto_random(ekey, sizeof(ekey));
    crypto_random(f->aesiv, sizeof(f->aesiv));
    bp_node_t *req = bp_new_dict();
    bp_dict_set(req, "ekey", bp_new_data(ekey, sizeof(ekey)));
    bp_dict_set(req, "eiv", bp_new_data(f->aesiv, 16));
    bp_dict_set(req, "timingProtocol", bp_new_string("NTP"));
    bp_dict_set(req, "timingPort", bp_new_uint(f->timing_port));
    bp_dict_set(req, "deviceID", bp_new_string("AA:BB:CC:DD:EE:FF"));
    bp_dict_set(req, "model", bp_new_string(model));
    bp_dict_set(req, "name", bp_new_string(name));
    bp_dict_set(req, "isScreenMirroringSession", bp_new_bool(true));
    bp_node_t *resp = NULL;
    int status = 0;
    if (request_plist(f, "SETUP", "rtsp://127.0.0.1/1234", req, &status, &resp) != 0) {
        return -1;
    }
    uint64_t timing = 0;
    bool ok = status == 200 && resp && bp_get_uint(bp_dict_get(resp, "timingPort"), &timing);
    bp_free(resp);
    if (!ok) {
        return status ? status : -1;
    }
    /* the receiver decrypts ekey with playfair; do the same to agree on the key */
    uint8_t message[164];
    uint8_t cipher[72];
    memcpy(message, f->keymsg, sizeof(message));
    memcpy(cipher, ekey, sizeof(cipher));
    playfair_decrypt(message, cipher, f->aeskey);
    if (f->have_shared) {
        uint8_t h[64];
        sha512_2(f->aeskey, 16, f->shared, 32, h);
        memcpy(f->aeskey, h, 16);
    }
    f->session = true;
    return 200;
}

static bp_node_t *first_stream(bp_node_t *resp) {
    return bp_array_get(bp_dict_get(resp, "streams"), 0);
}

int fs_setup_mirror(fake_sender_t *f, uint64_t stream_connection_id) {
    bp_node_t *stream = bp_new_dict();
    bp_dict_set(stream, "type", bp_new_uint(110));
    bp_dict_set(stream, "streamConnectionID", bp_new_uint(stream_connection_id));
    bp_dict_set(stream, "timestampInfo", bp_new_array());
    bp_node_t *streams = bp_new_array();
    bp_array_append(streams, stream);
    bp_node_t *req = bp_new_dict();
    bp_dict_set(req, "streams", streams);
    bp_node_t *resp = NULL;
    int status = 0;
    if (request_plist(f, "SETUP", "rtsp://127.0.0.1/1234", req, &status, &resp) != 0 || status != 200) {
        bp_free(resp);
        return -1;
    }
    uint64_t port = 0;
    bool ok = bp_get_uint(bp_dict_get(first_stream(resp), "dataPort"), &port) && port > 0 && port < 65536;
    bp_free(resp);
    if (!ok) {
        return -1;
    }
    struct sockaddr_in a = f->server;
    a.sin_port = htons((uint16_t) port);
    f->mirror_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (f->mirror_fd < 0 || connect(f->mirror_fd, (struct sockaddr *) &a, sizeof(a)) != 0) {
        return -1;
    }
    uint8_t key[16];
    uint8_t iv[16];
    mirror_derive_keys(f->aeskey, stream_connection_id, key, iv);
    f->ctr_ready = aes_ctr_init(&f->ctr, key, iv) == 0;
    return f->ctr_ready ? 0 : -1;
}

int fs_setup_audio(fake_sender_t *f, int ct, int spf) {
    f->control_fd = udp_socket(&f->control_port);
    if (f->control_fd < 0) {
        return -1;
    }
    bp_node_t *stream = bp_new_dict();
    bp_dict_set(stream, "type", bp_new_uint(96));
    bp_dict_set(stream, "ct", bp_new_uint((uint64_t) ct));
    bp_dict_set(stream, "spf", bp_new_uint((uint64_t) spf));
    bp_dict_set(stream, "controlPort", bp_new_uint(f->control_port));
    bp_dict_set(stream, "audioFormat", bp_new_uint(0x40000));
    bp_dict_set(stream, "isMedia", bp_new_bool(true));
    bp_node_t *streams = bp_new_array();
    bp_array_append(streams, stream);
    bp_node_t *req = bp_new_dict();
    bp_dict_set(req, "streams", streams);
    bp_node_t *resp = NULL;
    int status = 0;
    if (request_plist(f, "SETUP", "rtsp://127.0.0.1/1234", req, &status, &resp) != 0 || status != 200) {
        bp_free(resp);
        return -1;
    }
    uint64_t dport = 0;
    uint64_t cport = 0;
    bp_node_t *s = first_stream(resp);
    bool ok = bp_get_uint(bp_dict_get(s, "dataPort"), &dport) && bp_get_uint(bp_dict_get(s, "controlPort"), &cport);
    bp_free(resp);
    if (!ok) {
        return -1;
    }
    f->audio_data = f->server;
    f->audio_data.sin_port = htons((uint16_t) dport);
    f->audio_control = f->server;
    f->audio_control.sin_port = htons((uint16_t) cport);
    f->audio_fd = socket(AF_INET, SOCK_DGRAM, 0);
    f->audio_seq = 1000;
    f->audio_rtp = 44100;
    return f->audio_fd >= 0 ? 0 : -1;
}

static void float_le(uint8_t *p, float v) {
    uint32_t u;
    memcpy(&u, &v, 4);
    p[0] = (uint8_t) u;
    p[1] = (uint8_t) (u >> 8);
    p[2] = (uint8_t) (u >> 16);
    p[3] = (uint8_t) (u >> 24);
}

static void u64_le(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t) (v >> (8 * i));
    }
}

int fs_send_codec(fake_sender_t *f, const uint8_t *config, size_t len, int width, int height, uint8_t option) {
    uint8_t header[128] = { 0 };
    header[0] = (uint8_t) len;
    header[1] = (uint8_t) (len >> 8);
    header[2] = (uint8_t) (len >> 16);
    header[3] = (uint8_t) (len >> 24);
    header[4] = 0x01;
    header[6] = option;
    header[7] = 0x01;
    u64_le(header + 8, ns_to_ntp(time_mono_ns()));
    float_le(header + 16, (float) width);
    float_le(header + 20, (float) height);
    float_le(header + 40, (float) width);
    float_le(header + 44, (float) height);
    float_le(header + 56, (float) width);
    float_le(header + 60, (float) height);
    if (send_all(f->mirror_fd, header, sizeof(header)) != 0) {
        return -1;
    }
    return len ? send_all(f->mirror_fd, config, len) : 0;
}

int fs_send_video(fake_sender_t *f, const uint8_t *avcc_au, size_t len, uint64_t ntp_timestamp) {
    uint8_t header[128] = { 0 };
    header[0] = (uint8_t) len;
    header[1] = (uint8_t) (len >> 8);
    header[2] = (uint8_t) (len >> 16);
    header[3] = (uint8_t) (len >> 24);
    header[4] = 0x00;
    u64_le(header + 8, ntp_timestamp);
    uint8_t *enc = (uint8_t *) malloc(len);
    if (!enc) {
        return -1;
    }
    aes_ctr_xcrypt(&f->ctr, avcc_au, enc, len);
    int r = send_all(f->mirror_fd, header, sizeof(header));
    if (r == 0) {
        r = send_all(f->mirror_fd, enc, len);
    }
    free(enc);
    return r;
}

int fs_send_audio(fake_sender_t *f, const uint8_t *payload, size_t len, uint32_t samples) {
    uint8_t pkt[2048];
    if (len + 12 > sizeof(pkt)) {
        return -1;
    }
    pkt[0] = 0x80;
    pkt[1] = 0x60;
    wr16be(pkt + 2, f->audio_seq);
    wr32be(pkt + 4, f->audio_rtp);
    wr32be(pkt + 8, 0);
    memcpy(pkt + 12, payload, len);
    size_t blocks = len & ~(size_t) 15;
    if (blocks) {
        mbedtls_aes_context aes;
        uint8_t iv[16];
        memcpy(iv, f->aesiv, 16);
        mbedtls_aes_init(&aes);
        mbedtls_aes_setkey_enc(&aes, f->aeskey, 128);
        mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, blocks, iv, pkt + 12, pkt + 12);
        mbedtls_aes_free(&aes);
    }
    ssize_t n = sendto(f->audio_fd, pkt, len + 12, 0, (struct sockaddr *) &f->audio_data, sizeof(f->audio_data));
    f->audio_seq++;
    f->audio_rtp += samples;
    return n == (ssize_t) (len + 12) ? 0 : -1;
}

int fs_send_sync(fake_sender_t *f) {
    uint8_t pkt[20];
    pkt[0] = 0x90;
    pkt[1] = 0xd4;
    pkt[2] = 0x00;
    pkt[3] = 0x04;
    wr32be(pkt + 4, f->audio_rtp);
    wr64be(pkt + 8, ns_to_ntp(time_mono_ns() + 2208988800ULL * 1000000000ULL));
    wr32be(pkt + 16, f->audio_rtp + 44100);
    ssize_t n = sendto(f->control_fd, pkt, sizeof(pkt), 0, (struct sockaddr *) &f->audio_control,
                       sizeof(f->audio_control));
    return n == (ssize_t) sizeof(pkt) ? 0 : -1;
}

int fs_set_volume(fake_sender_t *f, float db) {
    char text[64];
    int n = snprintf(text, sizeof(text), "volume: %.6f\r\n", (double) db);
    int status = 0;
    return fs_request(f, "SET_PARAMETER", "rtsp://127.0.0.1/1234", "text/parameters", text, (size_t) n, &status,
                      NULL, NULL) == 0 && status == 200 ? 0 : -1;
}

int fs_set_track(fake_sender_t *f, const char *title, const char *artist, const char *album) {
    /* mlit { minm, asar, asal }, as iOS sends it */
    const char *tags[3] = { "minm", "asar", "asal" };
    const char *values[3] = { title, artist, album };
    uint8_t body[1024];
    size_t n = 8;
    for (int i = 0; i < 3; i++) {
        size_t len = strlen(values[i]);
        if (n + 8 + len > sizeof(body)) {
            return -1;
        }
        memcpy(body + n, tags[i], 4);
        wr32be(body + n + 4, (uint32_t) len);
        memcpy(body + n + 8, values[i], len);
        n += 8 + len;
    }
    memcpy(body, "mlit", 4);
    wr32be(body + 4, (uint32_t) (n - 8));
    return fs_set_parameter(f, "application/x-dmap-tagged", body, n);
}

int fs_set_progress(fake_sender_t *f, uint32_t start, uint32_t current, uint32_t end) {
    char text[64];
    int n = snprintf(text, sizeof(text), "progress: %u/%u/%u\r\n", start, current, end);
    return fs_set_parameter(f, "text/parameters", text, (size_t) n);
}

int fs_set_parameter(fake_sender_t *f, const char *content_type, const void *data, size_t len) {
    int status = 0;
    return fs_request(f, "SET_PARAMETER", "rtsp://127.0.0.1/1234", content_type, data, len, &status, NULL, NULL) == 0 &&
                   status == 200 ? 0 : -1;
}

int fs_feedback(fake_sender_t *f) {
    int status = 0;
    return fs_request(f, "POST", "/feedback", NULL, NULL, 0, &status, NULL, NULL) == 0 && status == 200 ? 0 : -1;
}

int fs_teardown(fake_sender_t *f, int type) {
    bp_node_t *req = NULL;
    if (type) {
        bp_node_t *stream = bp_new_dict();
        bp_dict_set(stream, "type", bp_new_uint((uint64_t) type));
        bp_node_t *streams = bp_new_array();
        bp_array_append(streams, stream);
        req = bp_new_dict();
        bp_dict_set(req, "streams", streams);
    }
    int status = 0;
    if (req) {
        return request_plist(f, "TEARDOWN", "rtsp://127.0.0.1/1234", req, &status, NULL) == 0 && status == 200 ? 0 : -1;
    }
    return fs_request(f, "TEARDOWN", "rtsp://127.0.0.1/1234", NULL, NULL, 0, &status, NULL, NULL) == 0 &&
                   status == 200 ? 0 : -1;
}

size_t fs_make_alac_frame(uint8_t *out, size_t cap, int phase) {
    const int samples = 352;
    size_t bits = 23 + (size_t) samples * 32;
    size_t bytes = (bits + 7) / 8;
    if (cap < bytes) {
        return 0;
    }
    memset(out, 0, bytes);
    size_t pos = 0;
#define PUT(value, n)                                                     \
    for (int b_ = (n) - 1; b_ >= 0; b_--, pos++) {                         \
        if (((uint32_t) (value) >> b_) & 1) out[pos / 8] |= (uint8_t) (0x80 >> (pos % 8)); \
    }
    PUT(1, 3);  /* stereo */
    PUT(0, 4);
    PUT(0, 12);
    PUT(0, 1);  /* no explicit size: max samples per frame */
    PUT(0, 2);  /* no uncompressed bytes */
    PUT(1, 1);  /* not compressed */
    for (int i = 0; i < samples; i++) {
        double t = (double) (phase * samples + i) / 44100.0;
        int16_t v = (int16_t) (8000.0 * sin(2.0 * 3.14159265358979 * 440.0 * t));
        PUT((uint16_t) v, 16);
        PUT((uint16_t) v, 16);
    }
#undef PUT
    return bytes;
}
