/* tests/test_sec_transport.c -- common/sec_transport.c over a socketpair (no relay): server key
 * persistence + stable fingerprint, handshake, both directions, stream chunking of a >60000-byte write,
 * pin verification, counter exhaustion refusal, and rejection of a garbage first message. */
#include "../common/sec_transport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } else printf("PASS: %s\n", m); } while (0)

static SecConn srv, cli;

/* client handshake needs the server's hello to already be in the socket buffer -- a socketpair buffers,
 * so: server_begin first, then the client handshake reads it and writes the ct, then the server consumes. */
static int connect_pair(SecVerifyFn v, void *u) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -9;
    if (sec_server_begin(&srv, sv[0]) != 0) return -8;
    if (sec_client_handshake(&cli, sv[1], v, u) != 0) return -7;
    for (int i = 0; i < 50 && !srv.established; i++) { sec_fill(&srv); static unsigned char t[SEC_MAX_PLAIN]; if (sec_next(&srv, t, sizeof(t)) < 0) return -6; }
    return srv.established ? 0 : -5;
}

static int accept_all(const unsigned char fp[32], void *u) { (void)fp; (void)u; return 0; }

int main(void) {
    unsigned char pin1[32], pin2[32], buf[SEC_MAX_PLAIN];
    char path[] = "/tmp/sectest_keyXXXXXX";
    int fd = mkstemp(path);
    close(fd); unlink(path);
    CHECK(sec_server_load_or_create_key(path, pin1) == 0, "server key generated and saved");
    CHECK(sec_server_load_or_create_key(path, pin2) == 0 && memcmp(pin1, pin2, 32) == 0, "reloading the key file gives the same fingerprint");
    { FILE *f = fopen(path, "rb"); long n = 0; if (f) { fseek(f, 0, SEEK_END); n = ftell(f); fclose(f); } CHECK(n == 3584, "key file is ek||dk (3584 bytes)"); }

    CHECK(connect_pair(accept_all, NULL) == 0, "handshake completes over a socketpair");
    CHECK(sec_send(&cli, "hello server\n", 13) == 0, "client sends");
    { long n; sec_fill(&srv); n = sec_next(&srv, buf, sizeof(buf)); CHECK(n == 13 && memcmp(buf, "hello server\n", 13) == 0, "server receives the plaintext"); }
    CHECK(sec_send(&srv, "hello client\n", 13) == 0, "server sends");
    { long n; sec_fill(&cli); n = sec_next(&cli, buf, sizeof(buf)); CHECK(n == 13 && memcmp(buf, "hello client\n", 13) == 0, "client receives the plaintext"); }

    { /* a 150000-byte compressible write spans 3 frames and arrives intact, in order */
        static unsigned char big[150000], got[150000];
        size_t total = 0; int i;
        for (i = 0; i < 150000; i++) big[i] = (unsigned char)("EDGE.GAME "[i % 10]);
        CHECK(sec_send(&cli, big, sizeof(big)) == 0, "150000-byte write accepted (chunked into frames)");
        for (i = 0; i < 200 && total < sizeof(got); i++) {
            long n = sec_next(&srv, buf, sizeof(buf));
            if (n < 0) break;
            if (n == 0) { if (sec_fill(&srv) <= 0) break; continue; }
            memcpy(got + total, buf, (size_t)n); total += (size_t)n;
        }
        CHECK(total == sizeof(big) && memcmp(got, big, sizeof(big)) == 0, "all 150000 bytes reassemble identically");
        CHECK(srv.rawlen < 100, "highly compressible stream used few wire bytes (no backlog)");
    }

    { unsigned char want_ok[32], want_bad[32]; SecPinPolicy p;
      memcpy(want_ok, pin1, 32); memcpy(want_bad, pin1, 32); want_bad[0] ^= 1;
      p.pin = want_ok; p.tofu_file = NULL; p.hostport = NULL;
      CHECK(connect_pair(sec_verify_pin_or_tofu, &p) == 0, "correct pin accepted");
      p.pin = want_bad;
      CHECK(connect_pair(sec_verify_pin_or_tofu, &p) == -7, "wrong pin rejected before the ciphertext is sent"); }

    CHECK(connect_pair(accept_all, NULL) == 0, "fresh pair for counter test");
    cli.send_ctr = SEC_REKEY_AT;
    CHECK(sec_send(&cli, "x", 1) == -1, "sender refuses once its counter is exhausted (forces a re-handshake)");
    cli.send_ctr = 0;
    srv.recv_ctr = SEC_REKEY_AT;
    CHECK(sec_send(&cli, "y", 1) == 0, "client can still send");
    { long n; sec_fill(&srv); n = sec_next(&srv, buf, sizeof(buf)); CHECK(n == -1, "receiver drops a connection whose counter is exhausted"); }

    { /* a client that sends garbage instead of an ML-KEM ct: the server derives (different) keys by
         implicit rejection, so the first real-looking frame fails authentication and the link drops */
      int sv[2]; unsigned char junk[4 + 1088], frame[4 + 64]; long n;
      socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
      sec_server_begin(&srv, sv[0]);
      memset(junk, 0x41, sizeof(junk)); junk[0] = 0x40; junk[1] = 0x04; junk[2] = 0; junk[3] = 0; /* len 1088 */
      send(sv[1], junk, sizeof(junk), 0);
      sec_fill(&srv);
      n = sec_next(&srv, buf, sizeof(buf));
      CHECK(n == 0 && srv.established, "garbage ct: server proceeds with implicit-rejection keys (no error oracle)");
      memset(frame, 0x42, sizeof(frame)); frame[0] = 64; frame[1] = frame[2] = frame[3] = 0;
      send(sv[1], frame, sizeof(frame), 0);
      sec_fill(&srv);
      n = sec_next(&srv, buf, sizeof(buf));
      CHECK(n == -1, "...and the first frame fails authentication, dropping the connection");
      { unsigned char wrongsize[4 + 10] = {10, 0, 0, 0}; int sv2[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, sv2);
        sec_server_begin(&srv, sv2[0]);
        send(sv2[1], wrongsize, sizeof(wrongsize), 0);
        sec_fill(&srv);
        CHECK(sec_next(&srv, buf, sizeof(buf)) == -1, "a handshake message of the wrong size is rejected outright"); } }
    unlink(path);
    printf("%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
