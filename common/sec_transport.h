/* common/sec_transport.h -- EDGE.GAME's encrypted, compressed socket transport (card #491).
 * A thin C host around PARENA's net/secure_channel (ML-KEM-768 handshake, LZ4, XChaCha20-Poly1305;
 * see PARENA/stdlib/net/secure_channel.prn for the protocol and its documented caveats). C owns only
 * what PARENA's v0 can't: sockets, length-prefixed message framing, counters, key storage.
 *
 * Wire: every message is [len LE32][bytes]. Server speaks first: "ES1"+ek (1187 B). Client replies
 * with the ML-KEM ciphertext (1088 B). After that each message is one sc-frame (see the .prn).
 * Stream semantics: plaintext is an opaque byte stream (the callers layer NDJSON lines on it); one
 * sec_send may span several frames, and frames are delivered in order.
 */
#ifndef EDGE_SEC_TRANSPORT_H
#define EDGE_SEC_TRANSPORT_H

#include <stddef.h>

#define SEC_MAX_PLAIN 60000
#define SEC_RAW_MAX 65536
#define SEC_PIN_BYTES 32
#define SEC_REKEY_AT 2147483600 /* frame counters are 32-bit: refuse past this so the peer must re-handshake (new keys) */

typedef struct {
    int fd;
    int is_server;
    int established;
    unsigned char send_key[32], recv_key[32];
    int send_ctr, recv_ctr;
    unsigned char raw[SEC_RAW_MAX];
    size_t rawlen;
} SecConn;

/* Load the server's ML-KEM keypair from path (ek||dk raw, 3584 B), generating + saving it (mode 0600)
 * if absent. Returns 0 on success; fingerprint (32 B) written to pin_out. */
int sec_server_load_or_create_key(const char *path, unsigned char pin_out[SEC_PIN_BYTES]);

/* Server: start a connection on an accepted fd (sends the hello). 0 ok, -1 error. */
int sec_server_begin(SecConn *c, int fd);

/* Client: blocking handshake on a connected fd. verify(fp, user) is called with the server's key
 * fingerprint BEFORE anything secret is sent; return 0 to accept. Returns 0 ok, -1 io error,
 * -2 rejected by verify / bad hello. */
typedef int (*SecVerifyFn)(const unsigned char fp[SEC_PIN_BYTES], void *user);
int sec_client_handshake(SecConn *c, int fd, SecVerifyFn verify, void *user);

/* Ready-made verifier: an explicit pin (32 B, or NULL) wins; otherwise trust-on-first-use against a
 * "host:port <fphex>" file (first contact is recorded and announced loudly on stderr; a later
 * mismatch is refused). */
typedef struct { const unsigned char *pin; const char *tofu_file; const char *hostport; } SecPinPolicy;
int sec_verify_pin_or_tofu(const unsigned char fp[SEC_PIN_BYTES], void *user);

/* One recv() into the internal buffer. 1 = got data, 0 = peer closed, -1 = error / would overflow. */
int sec_fill(SecConn *c);

/* Next complete message, decrypted. Returns plaintext length (>0, copied to out, cap >= SEC_MAX_PLAIN),
 * 0 = nothing complete yet, -1 = protocol/authentication failure (drop the connection). During the
 * server's handshake this consumes the client's ct and returns 0 once keys are ready. */
long sec_next(SecConn *c, unsigned char *out, size_t cap);

/* Encrypt + send (blocking write loop), chunking into <= SEC_MAX_PLAIN frames. 0 ok, -1 error. */
int sec_send(SecConn *c, const void *data, size_t n);

void sec_hex(const unsigned char *in, size_t n, char *out);          /* out >= 2n+1 */
int sec_unhex(const char *hex, unsigned char *out, size_t n);        /* 0 ok */

#endif
