/* common/sec_transport.c -- see sec_transport.h. The parena runtime header must come first (it sets
 * the feature-test macros before any system header; same rule edge_client.c documents). */
#include "../vendor/sc/sc_gen.c"
#include "sec_transport.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static KemKeyPair g_srv_kp;
static Arena g_srv_arena;
static int g_srv_ready = 0;

/* All PARENA calls use a short-lived arena so long-running relays don't grow. */
static Bytes mk(Arena *a, const unsigned char *p, size_t n) {
    Bytes b = bytes_alloc_impl(a, (int)n);
    if (n) memcpy(b.data, p, n);
    return b;
}

void sec_hex(const unsigned char *in, size_t n, char *out) {
    static const char *h = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) { out[2 * i] = h[in[i] >> 4]; out[2 * i + 1] = h[in[i] & 15]; }
    out[2 * n] = '\0';
}

int sec_unhex(const char *hex, unsigned char *out, size_t n) {
    size_t i;
    if (strlen(hex) != 2 * n) return -1;
    for (i = 0; i < n; i++) {
        unsigned v = 0; int k;
        for (k = 0; k < 2; k++) {
            char ch = hex[2 * i + (size_t)k];
            unsigned d;
            if (ch >= '0' && ch <= '9') d = (unsigned)(ch - '0');
            else if (ch >= 'a' && ch <= 'f') d = (unsigned)(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F') d = (unsigned)(ch - 'A' + 10);
            else return -1;
            v = v * 16 + d;
        }
        out[i] = (unsigned char)v;
    }
    return 0;
}

int sec_server_load_or_create_key(const char *path, unsigned char pin_out[SEC_PIN_BYTES]) {
    static unsigned char blob[1184 + 2400];
    FILE *f = fopen(path, "rb");
    Arena tmp;
    Bytes pin;
    if (f) {
        size_t got = fread(blob, 1, sizeof(blob), f);
        fclose(f);
        if (got != sizeof(blob)) { fprintf(stderr, "sec: %s is not a 3584-byte ML-KEM keypair\n", path); return -1; }
    } else {
        Arena a; Bytes ek, dk;
        arena_init(&a);
        KemKeyPair kp = mlkem_keygen(&a);
        ek = kp.encapsulation_key; dk = kp.decapsulation_key;
        if (ek.len != 1184 || dk.len != 2400) { arena_free_all(&a); return -1; }
        memcpy(blob, ek.data, 1184);
        memcpy(blob + 1184, dk.data, 2400);
        arena_free_all(&a);
        f = fopen(path, "wb");
        if (!f) { fprintf(stderr, "sec: cannot write %s\n", path); return -1; }
#ifndef _WIN32
        fchmod(fileno(f), 0600);
#endif
        fwrite(blob, 1, sizeof(blob), f);
        fclose(f);
    }
    if (g_srv_ready) arena_free_all(&g_srv_arena);
    arena_init(&g_srv_arena);
    g_srv_kp.encapsulation_key = mk(&g_srv_arena, blob, 1184);
    g_srv_kp.decapsulation_key = mk(&g_srv_arena, blob + 1184, 2400);
    g_srv_ready = 1;
    arena_init(&tmp);
    pin = sc_fingerprint(g_srv_kp.encapsulation_key, &tmp);
    if (pin.len != 32) { arena_free_all(&tmp); return -1; }
    memcpy(pin_out, pin.data, 32);
    arena_free_all(&tmp);
    return 0;
}

static int send_all(int fd, const unsigned char *p, size_t n) {
    while (n > 0) {
        int w = (int)send((
#ifdef _WIN32
            SOCKET
#else
            int
#endif
            )fd, (const char *)p, (int)n, 0);
        if (w <= 0) return -1;
        p += w; n -= (size_t)w;
    }
    return 0;
}

static int send_msg(int fd, const unsigned char *p, size_t n) {
    unsigned char hdr[4];
    hdr[0] = (unsigned char)(n & 255); hdr[1] = (unsigned char)((n >> 8) & 255);
    hdr[2] = (unsigned char)((n >> 16) & 255); hdr[3] = (unsigned char)((n >> 24) & 255);
    if (send_all(fd, hdr, 4) < 0) return -1;
    return send_all(fd, p, n);
}

int sec_server_begin(SecConn *c, int fd) {
    unsigned char hello[3 + 1184];
    memset(c, 0, sizeof(*c));
    c->fd = fd; c->is_server = 1;
    if (!g_srv_ready) return -1;
    hello[0] = 'E'; hello[1] = 'S'; hello[2] = '1';
    memcpy(hello + 3, g_srv_kp.encapsulation_key.data, 1184);
    return send_msg(fd, hello, sizeof(hello));
}

static int recv_exact(int fd, unsigned char *p, size_t n) {
    while (n > 0) {
        int r = (int)recv((
#ifdef _WIN32
            SOCKET
#else
            int
#endif
            )fd, (char *)p, (int)n, 0);
        if (r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

static void derive(SecConn *c, Bytes ss, Arena *a) {
    int cli = !c->is_server;
    Bytes k1 = sc_derive_key(ss, 1, a), k2 = sc_derive_key(ss, 2, a);
    memcpy(cli ? c->send_key : c->recv_key, k1.data, 32); /* c2s */
    memcpy(cli ? c->recv_key : c->send_key, k2.data, 32); /* s2c */
    c->established = 1;
}

int sec_client_handshake(SecConn *c, int fd, SecVerifyFn verify, void *user) {
    unsigned char hdr[4], hello[3 + 1184];
    size_t len;
    Arena a;
    int rc = -2;
    memset(c, 0, sizeof(*c));
    c->fd = fd;
    if (recv_exact(fd, hdr, 4) < 0) return -1;
    len = (size_t)hdr[0] | ((size_t)hdr[1] << 8) | ((size_t)hdr[2] << 16) | ((size_t)hdr[3] << 24);
    if (len != sizeof(hello)) return -2;
    if (recv_exact(fd, hello, sizeof(hello)) < 0) return -1;
    if (hello[0] != 'E' || hello[1] != 'S' || hello[2] != '1') return -2;
    arena_init(&a);
    {
        Bytes ek = mk(&a, hello + 3, 1184);
        Bytes fp = sc_fingerprint(ek, &a);
        if (fp.len != 32) goto done;
        if (verify && verify(fp.data, user) != 0) goto done;
        {
            KemEncaps e = mlkem_encaps(ek, &a);
            if (e.ciphertext.len != 1088 || e.shared_secret.len != 32) goto done;
            if (send_msg(fd, e.ciphertext.data, 1088) < 0) { rc = -1; goto done; }
            derive(c, e.shared_secret, &a);
            rc = 0;
        }
    }
done:
    arena_free_all(&a);
    return rc;
}

int sec_verify_pin_or_tofu(const unsigned char fp[SEC_PIN_BYTES], void *user) {
    SecPinPolicy *pol = (SecPinPolicy *)user;
    char hex[2 * SEC_PIN_BYTES + 1];
    sec_hex(fp, SEC_PIN_BYTES, hex);
    if (pol->pin) {
        if (memcmp(pol->pin, fp, SEC_PIN_BYTES) == 0) return 0;
        fprintf(stderr, "[sec] REFUSING: server fingerprint %s does not match the pinned one\n", hex);
        return -1;
    }
    if (pol->tofu_file && pol->hostport) {
        char line[256], host[160], known[160];
        FILE *f = fopen(pol->tofu_file, "r");
        if (f) {
            while (fgets(line, sizeof(line), f)) {
                if (sscanf(line, "%159s %159s", host, known) == 2 && strcmp(host, pol->hostport) == 0) {
                    fclose(f);
                    if (strcmp(known, hex) == 0) return 0;
                    fprintf(stderr, "[sec] REFUSING: %s presented fingerprint %s but %s remembers %s\n"
                                    "[sec] (if the relay's key was deliberately replaced, delete that line)\n",
                            pol->hostport, hex, pol->tofu_file, known);
                    return -1;
                }
            }
            fclose(f);
        }
        f = fopen(pol->tofu_file, "a");
        if (f) { fprintf(f, "%s %s\n", pol->hostport, hex); fclose(f); }
        fprintf(stderr, "[sec] FIRST CONTACT with %s -- trusting and remembering fingerprint %s\n"
                        "[sec] (verify it against the relay's startup log, or pass EDGE_SERVER_PIN)\n", pol->hostport, hex);
        return 0;
    }
    fprintf(stderr, "[sec] no pin and no TOFU file configured; refusing %s\n", hex);
    return -1;
}

int sec_fill(SecConn *c) {
    int r;
    if (c->rawlen >= sizeof(c->raw)) return -1;
    r = (int)recv((
#ifdef _WIN32
        SOCKET
#else
        int
#endif
        )c->fd, (char *)c->raw + c->rawlen, (int)(sizeof(c->raw) - c->rawlen), 0);
    if (r == 0) return 0;
    if (r < 0) return -1;
    c->rawlen += (size_t)r;
    return 1;
}

long sec_next(SecConn *c, unsigned char *out, size_t cap) {
    size_t len, total;
    Arena a;
    long result = -1;
    if (c->rawlen < 4) return 0;
    len = (size_t)c->raw[0] | ((size_t)c->raw[1] << 8) | ((size_t)c->raw[2] << 16) | ((size_t)c->raw[3] << 24);
    if (len > sizeof(c->raw) - 4) return -1;
    total = 4 + len;
    if (c->rawlen < total) return 0;
    arena_init(&a);
    if (!c->established) {
        if (!c->is_server || len != 1088) goto done;
        {
            Bytes ct = mk(&a, c->raw + 4, 1088);
            Bytes ss = mlkem_decaps(ct, g_srv_kp.decapsulation_key, &a);
            if (ss.len != 32) goto done;
            derive(c, ss, &a);
        }
        result = 0;
    } else {
        Bytes wire = mk(&a, c->raw + 4, len);
        Bytes plain = sc_frame_open(mk(&a, c->recv_key, 32), c->is_server ? 1 : 2, c->recv_ctr, wire, &a);
        if (plain.len <= 0 || (size_t)plain.len > cap) goto done;
        memcpy(out, plain.data, (size_t)plain.len);
        c->recv_ctr++;
        result = plain.len;
    }
    memmove(c->raw, c->raw + total, c->rawlen - total);
    c->rawlen -= total;
done:
    arena_free_all(&a);
    return result;
}

int sec_send(SecConn *c, const void *data, size_t n) {
    const unsigned char *p = (const unsigned char *)data;
    if (!c->established) return -1;
    while (n > 0) {
        size_t chunk = n > SEC_MAX_PLAIN ? SEC_MAX_PLAIN : n;
        Arena a;
        Bytes wire;
        int rc;
        arena_init(&a);
        wire = sc_frame_seal(mk(&a, c->send_key, 32), c->is_server ? 2 : 1, c->send_ctr, mk(&a, p, chunk), &a);
        if (wire.len <= 0) { arena_free_all(&a); return -1; }
        rc = send_msg(c->fd, wire.data, (size_t)wire.len);
        arena_free_all(&a);
        if (rc < 0) return -1;
        c->send_ctr++;
        p += chunk; n -= chunk;
    }
    return 0;
}
