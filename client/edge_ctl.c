/* client/edge_ctl.c -- operator/device command line for the EDGE.GAME relay (card #491).
 * Speaks the same encrypted transport as edge_client (common/sec_transport.h), so it works from the
 * founder's Windows PC, the Pi (`boot_announce.sh` pushes its boot event with it), and the e2e test.
 *
 *   edge_ctl <host> <port> <token> [--pin HEX] [--wait SECS] [--follow SECS] [--stdin] [json-line ...]
 *
 * Handshake + hello are done for you; each json-line argument is sent as one NDJSON line; every line
 * the relay sends back is printed to stdout, one per line. Output stops after --wait seconds of
 * silence (default 1.5) or, with --follow, after that many seconds in total (for live event streams).
 * --stdin (POSIX only): also send every line read from stdin, interactively, until --follow expires --
 * this is how scripts and Claude hold one live session open.
 * Exit code: 0 ok, 1 usage/connect, 2 handshake rejected (pin/TOFU), 3 hello refused.
 * Server fingerprint: --pin HEX / EDGE_SERVER_PIN, else trust-on-first-use in ./edge_known_servers.txt. */
#include "parena_runtime.h"
#include "../common/sec_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define CLOSESOCK closesocket
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int sock_t;
#define CLOSESOCK close
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#endif

static SecConn g_sec;

static int send_json(const char *json) {
    static char buf[SEC_MAX_PLAIN];
    int n = snprintf(buf, sizeof(buf), "%s\n", json);
    if (n < 0 || (size_t)n >= sizeof(buf)) return -1;
    return sec_send(&g_sec, buf, (size_t)n);
}

/* Print every complete line in `acc`, keeping any partial tail. Returns number of lines printed. */
static int print_lines(char *acc, size_t *len) {
    int lines = 0;
    char *start = acc, *nl;
    acc[*len] = '\0';
    while ((nl = strchr(start, '\n')) != NULL) {
        *nl = '\0';
        if (*start) { printf("%s\n", start); lines++; }
        start = nl + 1;
    }
    *len = strlen(start);
    memmove(acc, start, *len + 1);
    fflush(stdout);
    return lines;
}

/* Wait up to `ms` for decrypted data; returns bytes appended to acc, 0 on timeout, -1 on close/error. */
static long read_some(char *acc, size_t *len, size_t cap, int ms) {
    static unsigned char plain[SEC_MAX_PLAIN];
    for (;;) {
        long n = sec_next(&g_sec, plain, sizeof(plain));
        if (n < 0) return -1;
        if (n > 0) {
            if (*len + (size_t)n >= cap) *len = 0;
            memcpy(acc + *len, plain, (size_t)n);
            *len += (size_t)n;
            return n;
        }
        {
            fd_set rf; struct timeval tv;
            int r;
            FD_ZERO(&rf);
            FD_SET((sock_t)g_sec.fd, &rf);
            tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
            r = select(g_sec.fd + 1, &rf, NULL, NULL, &tv);
            if (r <= 0) return 0;
            if (sec_fill(&g_sec) <= 0) return -1;
        }
    }
}

int main(int argc, char **argv) {
    const char *host, *token, *pin_hex = getenv("EDGE_SERVER_PIN");
    int port, i, wait_ms = 1500, follow_ms = 0, first_json = -1, use_stdin = 0;
    static char acc[SEC_MAX_PLAIN * 2];
    size_t acclen = 0;
    sock_t s;
    struct sockaddr_in addr;
    unsigned char pin[SEC_PIN_BYTES];
    SecPinPolicy pol;
    char hostport[200];

    if (argc < 4) { fprintf(stderr, "usage: %s <host> <port> <token> [--pin HEX] [--wait SECS] [--follow SECS] [json ...]\n", argv[0]); return 1; }
    host = argv[1]; port = atoi(argv[2]); token = argv[3];
    for (i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--pin") == 0 && i + 1 < argc) pin_hex = argv[++i];
        else if (strcmp(argv[i], "--wait") == 0 && i + 1 < argc) wait_ms = (int)(atof(argv[++i]) * 1000);
        else if (strcmp(argv[i], "--follow") == 0 && i + 1 < argc) follow_ms = (int)(atof(argv[++i]) * 1000);
        else if (strcmp(argv[i], "--stdin") == 0) use_stdin = 1;
        else { first_json = i; break; }
    }
#ifdef _WIN32
    { WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa); }
#endif
    s = socket(AF_INET, SOCK_STREAM, 0);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (s == INVALID_SOCKET || inet_pton(AF_INET, host, &addr.sin_addr) != 1 ||
        connect(s, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "connect to %s:%d failed\n", host, port);
        return 1;
    }
    snprintf(hostport, sizeof(hostport), "%s:%d", host, port);
    pol.pin = (pin_hex && *pin_hex && sec_unhex(pin_hex, pin, sizeof(pin)) == 0) ? pin : NULL;
    if (pin_hex && *pin_hex && !pol.pin) { fprintf(stderr, "pin must be 64 hex chars\n"); return 1; }
    pol.tofu_file = "edge_known_servers.txt";
    pol.hostport = hostport;
    if (sec_client_handshake(&g_sec, (int)s, sec_verify_pin_or_tofu, &pol) != 0) { fprintf(stderr, "secure handshake rejected\n"); return 2; }

    {
        char hello[600];
        snprintf(hello, sizeof(hello), "{\"type\":\"hello\",\"token\":\"%s\"}", token);
        if (send_json(hello) < 0) return 1;
        if (read_some(acc, &acclen, sizeof(acc), 3000) <= 0 || !strstr(acc, "hello_ok")) {
            fprintf(stderr, "hello refused (wrong token?)\n");
            return 3;
        }
        print_lines(acc, &acclen);
    }
    if (first_json > 0) for (i = first_json; i < argc; i++) if (send_json(argv[i]) < 0) return 1;

#ifndef _WIN32
    if (use_stdin) {
        static char inbuf[SEC_MAX_PLAIN];
        size_t inlen = 0;
        int stdin_open = 1;
        long left = follow_ms > 0 ? follow_ms : 3600000L;
        while (left > 0) {
            fd_set rf; struct timeval tv; int maxfd = g_sec.fd, r;
            /* drain anything already decrypted before blocking */
            long got = read_some(acc, &acclen, sizeof(acc), 0);
            if (got < 0) break;
            print_lines(acc, &acclen);
            FD_ZERO(&rf);
            FD_SET(g_sec.fd, &rf);
            if (stdin_open) { FD_SET(0, &rf); if (0 > maxfd) maxfd = 0; }
            tv.tv_sec = 0; tv.tv_usec = 200000;
            r = select(maxfd + 1, &rf, NULL, NULL, &tv);
            left -= 200;
            if (r <= 0) continue;
            if (stdin_open && FD_ISSET(0, &rf)) {
                long n = (long)read(0, inbuf + inlen, sizeof(inbuf) - inlen - 1);
                if (n <= 0) stdin_open = 0;
                else {
                    char *start = inbuf, *nl;
                    inlen += (size_t)n; inbuf[inlen] = '\0';
                    while ((nl = strchr(start, '\n')) != NULL) { *nl = '\0'; if (*start) send_json(start); start = nl + 1; }
                    inlen = strlen(start); memmove(inbuf, start, inlen + 1);
                }
            }
        }
        CLOSESOCK(s);
        return 0;
    }
#else
    (void)use_stdin;
#endif
    if (follow_ms > 0) {
        long left = follow_ms;
        while (left > 0) {
            long r = read_some(acc, &acclen, sizeof(acc), left > 200 ? 200 : (int)left);
            if (r < 0) break;
            print_lines(acc, &acclen);
            left -= 200;
        }
    } else {
        for (;;) {
            long r = read_some(acc, &acclen, sizeof(acc), wait_ms);
            if (r <= 0) break;
            print_lines(acc, &acclen);
        }
    }
    CLOSESOCK(s);
    return 0;
}
