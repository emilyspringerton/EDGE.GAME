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
#include "serial_port.h"
#include "avr109.h"
#include "../common/sec_transport.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define CLOSESOCK closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
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

/* The one encrypted connection (ML-KEM-768 + LZ4 + XChaCha20-Poly1305, common/sec_transport.h);
 * every line the client sends goes through it, so nothing below touches the raw socket again. */
static SecConn g_sec;

static int send_line(sock_t s, const char *json) {
    size_t len = strlen(json);
    char *buf = (char *)malloc(len + 2); /* heap: editor_get / logs can exceed any fixed stack buffer */
    int rc;
    (void)s;
    if (!buf) return 0;
    memcpy(buf, json, len);
    buf[len] = '\n';
    rc = sec_send(&g_sec, buf, len + 1) == 0;
    free(buf);
    return rc;
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


/* ---- serial / terminal capture (cards #492/#474) ----------------------------------------------
 * The operator can open the Feather's COM port ("auto" finds it with the USB probe), write to it, and
 * read everything it prints: each line arrives at the relay as an unsolicited
 * {"type":"log","channel":"serial","data":...} the relay buffers and streams -- so Claude reads the
 * Feather's output directly instead of the founder pasting it. */
static SerialPort *g_sp = NULL;
static char g_sp_name[64];
static char g_sp_line[512];
static size_t g_sp_len = 0;
static unsigned long g_sp_idle_polls = 0;

/* json_escape -- escape arbitrary bytes into a JSON string body (control + non-ASCII bytes -> \u00XX). */
static void json_escape_bytes(const unsigned char *in, size_t n, char *out, size_t cap) {
    size_t o = 0, i;
    for (i = 0; i < n && o + 8 < cap; i++) {
        unsigned char c = in[i];
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
        else if (c == '\t') { out[o++] = '\\'; out[o++] = 't'; }
        else if (c < 0x20 || c > 0x7e) { o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c); }
        else out[o++] = (char)c;
    }
    out[o] = '\0';
}

/* extract_str_unescaped -- like extract_str but understands \" \\ \n \r \t \uXXXX (<= 0xFF). */
static int extract_str_unescaped_n(const char *json, const char *field, char *out, size_t outlen);
static int extract_str_unescaped(const char *json, const char *field, char *out, size_t outlen) {
    return extract_str_unescaped_n(json, field, out, outlen);
}
static int extract_str_unescaped_n(const char *json, const char *field, char *out, size_t outlen) {
    char needle[64];
    const char *p;
    size_t o = 0;
    snprintf(needle, sizeof(needle), "\"%s\":\"", field);
    p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p && *p != '"' && o + 1 < outlen) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n') out[o++] = '\n';
            else if (*p == 'r') out[o++] = '\r';
            else if (*p == 't') out[o++] = '\t';
            else if (*p == 'u' && p[1] && p[2] && p[3] && p[4]) {
                char hx[5]; memcpy(hx, p + 1, 4); hx[4] = 0;
                out[o++] = (char)strtol(hx, NULL, 16);
                p += 4;
            } else out[o++] = *p;
            p++;
        } else out[o++] = *p++;
    }
    out[o] = '\0';
    return 1;
}

static void emit_log(sock_t s, const char *channel, const unsigned char *data, size_t n) {
    char esc[1200], line[1400];
    json_escape_bytes(data, n, esc, sizeof(esc));
    snprintf(line, sizeof(line), "{\"type\":\"log\",\"channel\":\"%s\",\"data\":\"%s\"}", channel, esc);
    send_line(s, line);
}

static void flush_serial_line(sock_t s) {
    if (g_sp_len > 0) { emit_log(s, "serial", (const unsigned char *)g_sp_line, g_sp_len); g_sp_len = 0; }
}

/* poll_serial -- called every loop tick: pull available bytes, emit full lines as they complete, and
 * flush a partial line (a prompt with no newline) after ~200ms of silence. */
static void poll_serial(sock_t s) {
    unsigned char chunk[256];
    int n;
    if (!g_sp) return;
    n = serial_read(g_sp, chunk, sizeof(chunk));
    if (n < 0) {
        char msg[160];
        snprintf(msg, sizeof(msg), "serial port %s read failed (unplugged?) -- closed", g_sp_name);
        emit_log(s, "client", (const unsigned char *)msg, strlen(msg));
        serial_close(g_sp); g_sp = NULL; g_sp_len = 0;
        return;
    }
    if (n == 0) { if (++g_sp_idle_polls >= 4) { flush_serial_line(s); g_sp_idle_polls = 0; } return; }
    g_sp_idle_polls = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = chunk[i];
        if (c == '\n') { if (g_sp_len && g_sp_line[g_sp_len - 1] == '\r') g_sp_len--; flush_serial_line(s); }
        else { g_sp_line[g_sp_len++] = (char)c; if (g_sp_len >= sizeof(g_sp_line) - 1) flush_serial_line(s); }
    }
}

static void handle_serial_open(sock_t s, const char *id, const char *line) {
    char port[64] = {0}, err[200] = "", resp[600];
    int baud = 115200;
    UsbDev devs[USB_PROBE_MAX];
    extract_str(line, "port", port, sizeof(port));
    extract_int(line, "baud", &baud);
    if (strcmp(port, "auto") == 0 || port[0] == '\0') {
        int n = usb_probe_enumerate(devs, USB_PROBE_MAX, "/sys"), i;
        port[0] = '\0';
        for (i = 0; i < n && !port[0]; i++)
            if (devs[i].role == USB_ROLE_FEATHER_APP || devs[i].role == USB_ROLE_FEATHER_BOOTLOADER)
                snprintf(port, sizeof(port), "%s", devs[i].port);
        if (!port[0]) snprintf(err, sizeof(err), "auto: no Feather found (run usb_probe for details)");
    }
    if (port[0] && !err[0]) {
        if (g_sp) { serial_close(g_sp); g_sp = NULL; g_sp_len = 0; }
        g_sp = serial_open(port, baud, err, sizeof(err));
        if (g_sp) snprintf(g_sp_name, sizeof(g_sp_name), "%s", port);
    }
    if (g_sp) snprintf(resp, sizeof(resp), "{\"id\":\"%s\",\"type\":\"serial_open_result\",\"ok\":true,\"port\":\"%s\",\"baud\":%d}", id, port, baud);
    else {
        char e2[400];
        json_escape_bytes((const unsigned char *)err, strlen(err), e2, sizeof(e2));
        snprintf(resp, sizeof(resp), "{\"id\":\"%s\",\"type\":\"serial_open_result\",\"ok\":false,\"error\":\"%s\"}", id, e2);
    }
    send_line(s, resp);
}

static void handle_serial_write(sock_t s, const char *id, const char *line) {
    char data[2048] = {0}, resp[300];
    int nl = 0, w = -1;
    extract_str_unescaped(line, "data", data, sizeof(data));
    extract_int(line, "newline", &nl);
    if (g_sp) {
        size_t n = strlen(data);
        if (nl && n + 1 < sizeof(data)) data[n++] = '\n';
        w = serial_write(g_sp, (const unsigned char *)data, n);
    }
    snprintf(resp, sizeof(resp), "{\"id\":\"%s\",\"type\":\"serial_write_result\",\"ok\":%s,\"bytes\":%d%s}", id,
             w >= 0 ? "true" : "false", w < 0 ? 0 : w, g_sp ? "" : ",\"error\":\"no serial port open (send serial_open first)\"");
    send_line(s, resp);
}


/* ---- flash_hex (cards #477/#474) ----------------------------------------------------------------
 * The SERVER compiles; this client only flashes. Payload {"hex":"<Intel HEX, JSON-escaped>"} and
 * optionally "bootloader_port" (flash that port directly, skipping the reset dance -- also the manual
 * override if the USB probe can't see the bootloader). Otherwise: 1200-baud touch on the Feather's
 * app port (closing any open capture first) -> wait for the Caterina bootloader to enumerate (239A:000C,
 * port number may CHANGE on Windows, so it is re-probed) -> AVR109 flash + verify -> wait for the sketch
 * to re-enumerate (239A:800C) and reopen the capture port. Progress streams as log channel "flash". */
static void flash_progress(void *user, const char *msg) { emit_log(*(sock_t *)user, "flash", (const unsigned char *)msg, strlen(msg)); }

static int find_port(UsbRole want, char *out, size_t cap) {
    UsbDev devs[USB_PROBE_MAX];
    int n = usb_probe_enumerate(devs, USB_PROBE_MAX, "/sys"), i;
    for (i = 0; i < n; i++) if (devs[i].role == want) { snprintf(out, cap, "%s", devs[i].port); return 1; }
    return 0;
}

static int wait_port(UsbRole want, char *out, size_t cap, int timeout_ms) {
    int waited = 0;
    while (!find_port(want, out, cap)) {
        if (waited >= timeout_ms) return 0;
#ifdef _WIN32
        Sleep(250);
#else
        usleep(250000);
#endif
        waited += 250;
    }
    return 1;
}

static void handle_flash_hex(sock_t s, const char *id, const char *line) {
    static unsigned char image[AVR_FLASH_MAX];
    char err[300] = "", bport[64] = "", app_port[64] = "", resp[700], e2[600];
    size_t hexcap = 262144;
    char *hex = (char *)malloc(hexcap);
    int len = -1, ok = 0, reopen_baud = 115200, was_open = g_sp != NULL;
    if (!hex) return;
    extract_str_unescaped_n(line, "hex", hex, hexcap);
    extract_str(line, "bootloader_port", bport, sizeof(bport));
    len = ihex_parse(hex, image, sizeof(image), err, sizeof(err));
    free(hex);
    if (len > 0) {
        SerialPort *bp = NULL;
        if (!bport[0]) {
            char port[64];
            if (find_port(USB_ROLE_FEATHER_BOOTLOADER, bport, sizeof(bport))) { /* already in the bootloader */ }
            else {
                if (g_sp) { snprintf(port, sizeof(port), "%s", g_sp_name); serial_close(g_sp); g_sp = NULL; g_sp_len = 0; }
                else if (!find_port(USB_ROLE_FEATHER_APP, port, sizeof(port))) { snprintf(err, sizeof(err), "no Feather found: run usb_probe (replug it, or double-tap RESET and pass bootloader_port)"); len = -1; }
                if (len > 0) {
                    SerialPort *t = serial_open(port, 1200, err, sizeof(err)); /* the 1200-baud touch */
                    if (!t) len = -1; else { serial_close(t); flash_progress(&s, "1200-baud touch sent, waiting for the bootloader"); }
                }
                if (len > 0 && !wait_port(USB_ROLE_FEATHER_BOOTLOADER, bport, sizeof(bport), 10000)) {
                    snprintf(err, sizeof(err), "the Feather did not re-enumerate as the Caterina bootloader within 10s");
                    len = -1;
                }
            }
        }
        if (len > 0) {
            bp = serial_open(bport, 57600, err, sizeof(err));
            if (!bp) len = -1;
            else {
                ok = avr109_flash(bp, image, len, flash_progress, &s, err, sizeof(err)) == 0;
                serial_close(bp);
            }
        }
    }
    if (ok) {
        flash_progress(&s, "waiting for the sketch to enumerate");
        if (wait_port(USB_ROLE_FEATHER_APP, app_port, sizeof(app_port), 10000) && was_open) {
            char e3[100];
            g_sp = serial_open(app_port, reopen_baud, e3, sizeof(e3));
            if (g_sp) snprintf(g_sp_name, sizeof(g_sp_name), "%s", app_port);
        }
        snprintf(resp, sizeof(resp), "{\"id\":\"%s\",\"type\":\"flash_result\",\"ok\":true,\"bytes\":%d,\"app_port\":\"%s\"}", id, len, app_port);
    } else {
        json_escape_bytes((const unsigned char *)err, strlen(err), e2, sizeof(e2));
        snprintf(resp, sizeof(resp), "{\"id\":\"%s\",\"type\":\"flash_result\",\"ok\":false,\"error\":\"%s\"}", id, e2);
    }
    send_line(s, resp);
}


/* ---- editor_set / editor_get (card #475) ---------------------------------------------------------
 * The relay's operator (Claude) edits the file the EDITOR.GAME window has open: editor_set writes it in
 * the client's working directory (the same directory the editor was launched in), and the editor picks
 * the change up by itself (EDITOR.GAME polls the file's mtime and reloads when its own buffer is clean).
 * editor_get reads the current text back. Names are restricted to a plain file name with a known
 * extension -- no paths, no "..", so a command can only touch editor documents next to the client. */
#define EDITOR_TEXT_MAX 48000
static int editor_name_ok(const char *n) {
    size_t i, len = strlen(n);
    const char *dot = strrchr(n, '.');
    if (len == 0 || len > 60 || !dot || dot == n) return 0;
    for (i = 0; i < len; i++) {
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return 0;
    }
    if (strstr(n, "..")) return 0;
    return strcmp(dot, ".prn") == 0 || strcmp(dot, ".ino") == 0 || strcmp(dot, ".c") == 0 || strcmp(dot, ".txt") == 0 || strcmp(dot, ".md") == 0;
}

static void editor_default_name(char *name, size_t cap) {
    const char *env = getenv("EDGE_EDITOR_FILE");
    snprintf(name, cap, "%s", env && *env ? env : "blink.prn");
}

static void handle_editor_set(sock_t s, const char *id, const char *line) {
    char name[80] = {0}, resp[500];
    char *text = (char *)malloc(EDITOR_TEXT_MAX + 1);
    size_t n;
    FILE *f;
    int ok = 0;
    const char *err = "";
    if (!text) return;
    if (!extract_str(line, "name", name, sizeof(name))) editor_default_name(name, sizeof(name));
    if (!editor_name_ok(name)) err = "bad name: plain file name with extension .prn/.ino/.c/.txt/.md only";
    else if (!extract_str_unescaped_n(line, "text", text, EDITOR_TEXT_MAX + 1)) err = "missing text";
    else if ((n = strlen(text)) >= EDITOR_TEXT_MAX) err = "text too large (limit 48000 bytes)";
    else if (!(f = fopen(name, "wb"))) err = "cannot write the file";
    else { fwrite(text, 1, n, f); fclose(f); ok = 1; }
    snprintf(resp, sizeof(resp), "{\"id\":\"%s\",\"type\":\"editor_set_result\",\"ok\":%s,\"name\":\"%s\",\"bytes\":%d,\"error\":\"%s\"}",
             id, ok ? "true" : "false", ok ? name : "", ok ? (int)strlen(text) : 0, err);
    send_line(s, resp);
    free(text);
}

static void handle_editor_get(sock_t s, const char *id, const char *line) {
    char name[80] = {0};
    FILE *f = NULL;
    unsigned char *raw = NULL;
    char *esc = NULL, *resp = NULL;
    size_t n = 0;
    if (!extract_str(line, "name", name, sizeof(name))) editor_default_name(name, sizeof(name));
    if (editor_name_ok(name) && (f = fopen(name, "rb")) != NULL) {
        raw = (unsigned char *)malloc(EDITOR_TEXT_MAX + 1);
        if (raw) { n = fread(raw, 1, EDITOR_TEXT_MAX, f); }
        fclose(f);
    }
    if (raw) {
        esc = (char *)malloc(n * 6 + 8);
        resp = (char *)malloc(n * 6 + 300);
    }
    if (raw && esc && resp) {
        json_escape_bytes(raw, n, esc, n * 6 + 8);
        snprintf(resp, n * 6 + 300, "{\"id\":\"%s\",\"type\":\"editor_get_result\",\"ok\":true,\"name\":\"%s\",\"text\":\"%s\"}", id, name, esc);
        send_line(s, resp);
    } else {
        char e[300];
        snprintf(e, sizeof(e), "{\"id\":\"%s\",\"type\":\"editor_get_result\",\"ok\":false,\"error\":\"file not found or bad name\"}", id);
        send_line(s, e);
    }
    free(raw); free(esc); free(resp);
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

    {
        unsigned char pin[SEC_PIN_BYTES];
        const char *pin_hex = argc > 4 ? argv[4] : getenv("EDGE_SERVER_PIN");
        char hostport[200];
        SecPinPolicy pol;
        snprintf(hostport, sizeof(hostport), "%s:%d", host, port);
        pol.pin = (pin_hex && *pin_hex && sec_unhex(pin_hex, pin, sizeof(pin)) == 0) ? pin : NULL;
        pol.tofu_file = "edge_known_servers.txt";
        pol.hostport = hostport;
        if (pin_hex && *pin_hex && !pol.pin) { fprintf(stderr, "EDGE_SERVER_PIN must be 64 hex chars\n"); return 1; }
        int hs = sec_client_handshake(&g_sec, (int)s, sec_verify_pin_or_tofu, &pol);
        if (hs != 0) { fprintf(stderr, "secure handshake failed (%d)\n", hs); return 1; }
    }

    char hello[512];
    snprintf(hello, sizeof(hello), "{\"type\":\"hello\",\"token\":\"%s\"}", token);
    send_line(s, hello);

    static char buf[70000];
    static unsigned char plain[SEC_MAX_PLAIN];
    size_t buflen = 0;
    for (;;) {
        long n = sec_next(&g_sec, plain, sizeof(plain));
        if (n < 0) { fprintf(stderr, "secure channel: authentication/protocol failure\n"); break; }
        if (n == 0) {
            /* idle: wait for socket data, but wake every 50ms while a serial port is open to poll it */
            fd_set rf; struct timeval tv; int r;
            FD_ZERO(&rf); FD_SET(s, &rf);
            tv.tv_sec = g_sp ? 0 : 1; tv.tv_usec = g_sp ? 50000 : 0;
            r = select((int)s + 1, &rf, NULL, NULL, &tv);
            if (r > 0 && sec_fill(&g_sec) <= 0) { fprintf(stderr, "connection closed\n"); break; }
            poll_serial(s);
            continue;
        }
        if (buflen + (size_t)n >= sizeof(buf)) buflen = 0; /* overflow-drop, never overrun */
        memcpy(buf + buflen, plain, (size_t)n);
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
                } else if (strcmp(type, "serial_open") == 0 && id[0] != '\0') {
                    handle_serial_open(s, id, line);
                } else if (strcmp(type, "serial_write") == 0 && id[0] != '\0') {
                    handle_serial_write(s, id, line);
                } else if (strcmp(type, "editor_set") == 0 && id[0] != '\0') {
                    handle_editor_set(s, id, line);
                } else if (strcmp(type, "editor_get") == 0 && id[0] != '\0') {
                    handle_editor_get(s, id, line);
                } else if (strcmp(type, "flash_hex") == 0 && id[0] != '\0') {
                    handle_flash_hex(s, id, line);
                } else if (strcmp(type, "serial_close") == 0 && id[0] != '\0') {
                    char resp[200];
                    if (g_sp) { serial_close(g_sp); g_sp = NULL; g_sp_len = 0; }
                    snprintf(resp, sizeof(resp), "{\"id\":\"%s\",\"type\":\"serial_close_result\",\"ok\":true}", id);
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
