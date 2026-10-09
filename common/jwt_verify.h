/* common/jwt_verify.h -- minimal ES256 JWT verification against a pinned IDUNA JWKS, just enough
 * to check that the operator connecting to the EDGE.GAME relay holds a real IDUNA-issued JWT
 * carrying a specific permission (card #599 follow-up: "relay-side JWT verification +
 * edge.game.operator enforcement", EMILY/BACKLOG.md EDGE-599-FOLLOWUP-1).
 *
 * Real, deliberate scope: this verifies the token's own ES256 signature and reads its
 * "permissions" claim locally -- it does NOT call back to IDUNA at request time (IDUNA's own
 * middleware.RequirePermission works the same way: trust the claim once the signature is good).
 * It does NOT check "exp" (JWTs from this handler are short-lived in practice -- 72h -- and there
 * is no clock-skew/replay budget decided yet; a real gap, named not hidden) and does NOT handle
 * JWKS key rotation (one pinned key, loaded once at startup from a local file an operator fetches
 * with `curl https://iam.okemily.com/.well-known/jwks.json` -- no live JWKS refresh, no "kid"
 * matching against multiple keys). Good enough to prove the real mechanism end to end; a second
 * pass should add exp checking and multi-key JWKS support before this is trusted beyond a single
 * operator's own token.
 */
#ifndef EDGE_JWT_VERIFY_H
#define EDGE_JWT_VERIFY_H

typedef struct JwtKey JwtKey;

/* Loads a JWKS JSON file (the raw bytes `curl .../.well-known/jwks.json` produces) and pins the
 * FIRST ES256/P-256 key found. Returns NULL on any parse/key-build failure (logged to stderr). */
JwtKey *jwt_key_load_file(const char *path);

/* Verifies `jwt` (a compact header.payload.signature string) against `key`. On success, returns 1
 * and copies the raw "permissions" JSON array's bytes (e.g. ["a","b"]) into perms_out (NUL-terminated,
 * truncated to fit). Returns 0 on any failure (bad format, bad base64, bad signature, no
 * "permissions" claim) -- callers must treat 0 as "not authorized", never partially trust a
 * failed verification. */
int jwt_verify_es256(JwtKey *key, const char *jwt, char *perms_out, int perms_cap);

/* True if `perms_json` (as produced by jwt_verify_es256) contains the exact string `perm`. */
int jwt_permissions_contains(const char *perms_json, const char *perm);

#endif
