/* common/jwt_verify.c -- see jwt_verify.h for scope and real, named limitations. */
#include "jwt_verify.h"

#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/bn.h>
#include <openssl/obj_mac.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct JwtKey {
    EC_KEY *ec;
};

/* base64url decode (no padding). Returns decoded length, or -1 on bad input. out must be at least
 * (inlen*3/4 + 3) bytes. Not constant-time -- decoding a PUBLIC signature/JWKS value, not a secret. */
static int b64url_decode(const char *in, int inlen, unsigned char *out, int outcap) {
    static const char *alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    unsigned char rev[256];
    int i, o = 0;
    unsigned int buf = 0;
    int bits = 0;
    memset(rev, 0xff, sizeof(rev));
    for (i = 0; i < 64; i++) rev[(unsigned char)alpha[i]] = (unsigned char)i;
    for (i = 0; i < inlen; i++) {
        unsigned char c = (unsigned char)in[i];
        unsigned char v;
        if (c == '=' ) continue; /* tolerate stray padding even though JWT/JWKS shouldn't have it */
        v = rev[c];
        if (v == 0xff) return -1;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= outcap) return -1;
            out[o++] = (unsigned char)((buf >> bits) & 0xff);
        }
    }
    return o;
}

/* find_json_str -- narrow extractor, same scope boundary as edge_client.c's own extract_str:
 * this is a self-controlled wire format (IDUNA's own JWKS/JWT, not a general JSON parser). Finds
 * "field":"value" and copies value (not unescaped -- JWKS x/y and JWT claim values used here are
 * plain base64url/ASCII, never containing a quote or backslash). */
static int find_json_str(const char *json, const char *field, char *out, int outcap) {
    char needle[64];
    const char *p;
    int n;
    snprintf(needle, sizeof(needle), "\"%s\":\"", field);
    p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    n = 0;
    while (p[n] && p[n] != '"' && n + 1 < outcap) { out[n] = p[n]; n++; }
    out[n] = '\0';
    return 1;
}

JwtKey *jwt_key_load_file(const char *path) {
    FILE *f = fopen(path, "rb");
    char *buf;
    long sz;
    char xs[64] = {0}, ys[64] = {0};
    unsigned char xb[48], yb[48];
    int xn, yn;
    EC_KEY *ec = NULL;
    BIGNUM *bx = NULL, *by = NULL;
    JwtKey *k;
    if (!f) { fprintf(stderr, "[jwt] cannot open JWKS file %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1 << 20) { fclose(f); fprintf(stderr, "[jwt] JWKS file bad size\n"); return NULL; }
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(buf); return NULL; }
    buf[sz] = '\0';
    fclose(f);

    if (!find_json_str(buf, "x", xs, sizeof(xs)) || !find_json_str(buf, "y", ys, sizeof(ys))) {
        fprintf(stderr, "[jwt] JWKS file has no EC x/y (expected an ES256/P-256 key)\n");
        free(buf);
        return NULL;
    }
    free(buf);

    xn = b64url_decode(xs, (int)strlen(xs), xb, (int)sizeof(xb));
    yn = b64url_decode(ys, (int)strlen(ys), yb, (int)sizeof(yb));
    if (xn != 32 || yn != 32) {
        fprintf(stderr, "[jwt] JWKS x/y decoded to wrong length (got %d/%d, want 32/32 for P-256)\n", xn, yn);
        return NULL;
    }

    ec = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    bx = BN_bin2bn(xb, 32, NULL);
    by = BN_bin2bn(yb, 32, NULL);
    if (!ec || !bx || !by || !EC_KEY_set_public_key_affine_coordinates(ec, bx, by)) {
        fprintf(stderr, "[jwt] failed to build EC public key from JWKS\n");
        if (ec) EC_KEY_free(ec);
        if (bx) BN_free(bx);
        if (by) BN_free(by);
        return NULL;
    }
    BN_free(bx);
    BN_free(by);

    k = (JwtKey *)malloc(sizeof(JwtKey));
    k->ec = ec;
    fprintf(stderr, "[jwt] loaded ES256/P-256 key from %s\n", path);
    return k;
}

int jwt_verify_es256(JwtKey *key, const char *jwt, char *perms_out, int perms_cap) {
    const char *dot1, *dot2;
    int header_len, payload_len, sig_b64_len;
    unsigned char digest[SHA256_DIGEST_LENGTH];
    unsigned char sig_raw[80];
    int sig_len;
    char *payload_json = NULL;
    BIGNUM *r, *s;
    ECDSA_SIG *sig;
    int ok = 0;
    const char *perm_key;
    int perm_body_len;

    if (perms_out && perms_cap > 0) perms_out[0] = '\0';
    if (!key || !key->ec || !jwt) return 0;

    dot1 = strchr(jwt, '.');
    if (!dot1) return 0;
    dot2 = strchr(dot1 + 1, '.');
    if (!dot2) return 0;
    header_len = (int)(dot1 - jwt);
    payload_len = (int)(dot2 - dot1 - 1);
    sig_b64_len = (int)strlen(dot2 + 1);
    if (header_len <= 0 || payload_len <= 0 || sig_b64_len <= 0) return 0;

    /* digest over the exact ASCII "header.payload" signing input, per JWS */
    SHA256((const unsigned char *)jwt, (size_t)(header_len + 1 + payload_len), digest);

    sig_len = b64url_decode(dot2 + 1, sig_b64_len, sig_raw, (int)sizeof(sig_raw));
    if (sig_len != 64) return 0; /* ES256 JWS signature is raw r(32)||s(32), not DER */

    r = BN_bin2bn(sig_raw, 32, NULL);
    s = BN_bin2bn(sig_raw + 32, 32, NULL);
    sig = ECDSA_SIG_new();
    if (!r || !s || !sig || !ECDSA_SIG_set0(sig, r, s)) {
        if (sig) ECDSA_SIG_free(sig);
        if (r) BN_free(r);
        if (s) BN_free(s);
        return 0;
    }
    ok = ECDSA_do_verify(digest, SHA256_DIGEST_LENGTH, sig, key->ec) == 1;
    ECDSA_SIG_free(sig); /* owns r and s now */
    if (!ok) return 0;

    /* signature is good -- decode the payload and read "permissions" out of it */
    payload_json = (char *)malloc((size_t)payload_len + 1);
    if (!payload_json) return 0;
    {
        int n = b64url_decode(dot1 + 1, payload_len, (unsigned char *)payload_json, payload_len + 1);
        if (n < 0) { free(payload_json); return 0; }
        payload_json[n] = '\0';
    }

    perm_key = strstr(payload_json, "\"permissions\":");
    if (!perm_key) { free(payload_json); return 0; }
    perm_key += strlen("\"permissions\":");
    while (*perm_key == ' ') perm_key++;
    if (*perm_key != '[') { free(payload_json); return 0; }
    {
        const char *close = strchr(perm_key, ']');
        if (!close) { free(payload_json); return 0; }
        perm_body_len = (int)(close - perm_key + 1);
    }
    if (perms_out) {
        int n = perm_body_len < perms_cap - 1 ? perm_body_len : perms_cap - 1;
        memcpy(perms_out, perm_key, (size_t)n);
        perms_out[n] = '\0';
    }
    free(payload_json);
    return 1;
}

int jwt_permissions_contains(const char *perms_json, const char *perm) {
    char needle[128];
    if (!perms_json || !perm) return 0;
    snprintf(needle, sizeof(needle), "\"%s\"", perm);
    return strstr(perms_json, needle) != NULL;
}
