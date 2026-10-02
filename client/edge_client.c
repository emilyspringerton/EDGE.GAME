/* EDGE.GAME cabinet client -- Phase 1 (see ../NORTHSTAR.md's own phased plan).
 *
 * Proves the wire protocol end to end with no real hardware: connects to the relay server over
 * plain TCP, authenticates, then answers commands the operator sends. A "route" command is
 * answered for real -- not just echoed -- by calling into route_for_source(), the REAL compiled
 * decision logic from PARENA/stdlib/edge_game/traffic_router.prn (Phase 0), proving the two
 * phases actually fit together, not just that each compiles alone. Any other command type gets a
 * generic ack. Real Win32 serial I/O / SDL2 UI / the embedded IDE are Phase 2+ -- this is
 * deliberately just the network+protocol+routing-decision skeleton, already written portable
 * (POSIX now, #ifdef _WIN32 for the mingw cross-compile in Phase 7) rather than deferred, so later
 * phases only add code, they don't rewrite this file.
 *
 * Not a general JSON client: extract_str/extract_int below are narrow, flat-field extractors for
 * this specific, self-controlled protocol (both ends are this same repo) -- correct here, not a
 * general-purpose parser, same real, deliberate scope boundary route_message's own PARENA-side
 * header comment draws for its own String-vs-general-JSON handling.
 *
 * Include order matters: traffic_router_gen.c pulls in the shared runtime/parena_runtime.h, which
 * must be the very first thing any translation unit includes -- it defines _POSIX_C_SOURCE
 * internally (needed for getaddrinfo/strtok_r/popen/cfmakeraw/etc. under this repo's own strict
 * -std=c99), and glibc's feature-test macros only take effect if set before ANY system header is
 * touched. Confirmed live: putting the ordinary <stdio.h>-style includes first (as this file
 * originally did) broke the build with a wall of "implicit declaration" errors under
 * -Wall -Wextra -pedantic -Werror -- fixed by including traffic_router_gen.c first, matching
 * PARENA's own tests/test_traffic_router.c's identical ordering. */
#include "traffic_router_gen.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usb_probe.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define CLOSESOCK closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int sock_t;
#define CLOSESOCK close
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#endif

static sock_t connect_to(const char *host, int port) {
    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) { CLOSESOCK(s); return INVALID_SOCKET; }
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) { CLOSESOCK(s); return INVALID_SOCKET; }
    return s;
}

static int send_line(sock_t s, const char *json) {
    char buf[16384];
    int n = snprintf(buf, sizeof(buf), "%s\n", json);
    if (n < 0 || (size_t)n >= sizeof(buf)) return 0;
    return send(s, buf, n, 0) == n;
}

/* extract_str -- pulls a bare "field":"value" string out of a flat JSON object. */
static int extract_str(const char *json, const char *field, char *out, size_t outlen) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":\"", field);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < outlen) out[i++] = *p++;
    out[i] = '\0';
    return 1;
}

/* extract_int -- pulls a bare "field":123 integer out of a flat (optionally nested) JSON object;
 * works for a field inside "payload":{...} the same way since it just scans for the literal key
 * text anywhere in the line, which is safe here because both ends of this protocol are this same
 * repo and never nest the same key name twice. */
static int extract_int(const char *json, const char *field, int *out) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", field);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ') p++;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return 0;
    *out = (int)v;
    return 1;
}

static const char *route_target_name(RouteTarget t) {
    switch (t.tag) {
        case RouteTarget_TAG_ToWindows: return "ToWindows";
        case RouteTarget_TAG_ToPi: return "ToPi";
        case RouteTarget_TAG_ToNano: return "ToNano";
        case RouteTarget_TAG_ToPiAndNano: return "ToPiAndNano";
    }
    return "Unknown";
}

/* probe_report -- run the USB/COM probe and write its JSON into out (card #493). */
static int probe_report(char *out, size_t outlen) {
    UsbDev devs[USB_PROBE_MAX];
    int n = usb_probe_enumerate(devs, USB_PROBE_MAX, "/sys");
    return usb_probe_json(devs, n, out, outlen);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "probe") == 0) { /* standalone: edge_client probe */
        char out[16384];
        if (probe_report(out, sizeof(out)) < 0) { fprintf(stderr, "probe output too large\n"); return 1; }
        printf("%s\n", out);
        return 0;
    }
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    int port = argc > 2 ? atoi(argv[2]) : 8091;
    const char *token = argc > 3 ? argv[3] : getenv("EDGE_CLIENT_TOKEN");
    if (!token) { fprintf(stderr, "usage: %s <host> <port> <token>\n", argv[0]); return 1; }

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    sock_t s = connect_to(host, port);
    if (s == INVALID_SOCKET) { fprintf(stderr, "connect failed\n"); return 1; }

    char hello[512];
    snprintf(hello, sizeof(hello), "{\"type\":\"hello\",\"token\":\"%s\"}", token);
    send_line(s, hello);

    char buf[4096];
    size_t buflen = 0;
    for (;;) {
        int n = recv(s, buf + buflen, sizeof(buf) - buflen - 1, 0);
        if (n <= 0) { fprintf(stderr, "connection closed\n"); break; }
        buflen += (size_t)n;
        buf[buflen] = '\0';

        char *line = buf;
        char *nl;
        while ((nl = strchr(line, '\n')) != NULL) {
            *nl = '\0';
            if (strlen(line) > 0) {
                char id[64] = {0}, type[64] = {0};
                extract_str(line, "id", id, sizeof(id));
                extract_str(line, "type", type, sizeof(type));
                fprintf(stderr, "[edge_client] received: %s\n", line);

                if (strcmp(type, "hello_ok") == 0) {
                    fprintf(stderr, "[edge_client] authenticated\n");
                } else if (strcmp(type, "route") == 0 && id[0] != '\0') {
                    int source = 0;
                    extract_int(line, "source", &source);
                    RouteTarget target = route_for_source(source);
                    char resp[256];
                    snprintf(resp, sizeof(resp),
                             "{\"id\":\"%s\",\"type\":\"route_result\",\"source\":%d,\"target\":\"%s\"}",
                             id, source, route_target_name(target));
                    send_line(s, resp);
                } else if (strcmp(type, "usb_probe") == 0 && id[0] != '\0') {
                    char pj[12000], resp[12400];
                    if (probe_report(pj, sizeof(pj)) < 0) snprintf(pj, sizeof(pj), "{\"error\":\"probe output too large\"}");
                    snprintf(resp, sizeof(resp), "{\"id\":\"%s\",\"type\":\"usb_probe_result\",%s", id, pj + 1);
                    send_line(s, resp);
                } else if (id[0] != '\0') {
                    char ack[512];
                    snprintf(ack, sizeof(ack), "{\"id\":\"%s\",\"type\":\"ack\",\"echo\":\"%s\"}", id, type);
                    send_line(s, ack);
                }
            }
            line = nl + 1;
        }
        size_t remaining = strlen(line);
        memmove(buf, line, remaining + 1);
        buflen = remaining;
    }

    CLOSESOCK(s);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
