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
#include <shellapi.h>
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

/* send_link -- tell the relay whether this host currently has the Feather's USB link (card #478: the
   Feather is on the Windows PC OR the Android tablet, never both). "Has the link" = its serial port is open. */
static void send_link(sock_t s) {
    char l[64];
    snprintf(l, sizeof(l), "{\"type\":\"link\",\"feather\":%d}", g_sp ? 1 : 0);
    send_line(s, l);
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
    send_link(s);
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
    send_link(s);
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

/* handle_file_get -- read a chunk of any file on the client host (HRIP: pull Hearthstone's own
 * bundle to the operator for format analysis; nothing is stored or published here). Same trust
 * model as exec: no path allowlist, gated only by the encrypted channel + IDUNA permission.
 * {"type":"file_get","path":"C:\\...","offset":N,"len":M<=30000} -> base64 chunk + total size. */
#define FILE_GET_MAX 30000
static void b64_encode(const unsigned char *in, size_t n, char *out) {
    static const char *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, o = 0;
    for (i = 0; i + 2 < n; i += 3) {
        unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8) | in[i + 2];
        out[o++] = a[(v >> 18) & 63]; out[o++] = a[(v >> 12) & 63]; out[o++] = a[(v >> 6) & 63]; out[o++] = a[v & 63];
    }
    if (n - i == 1) {
        unsigned v = (unsigned)in[i] << 16;
        out[o++] = a[(v >> 18) & 63]; out[o++] = a[(v >> 12) & 63]; out[o++] = '='; out[o++] = '=';
    } else if (n - i == 2) {
        unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8);
        out[o++] = a[(v >> 18) & 63]; out[o++] = a[(v >> 12) & 63]; out[o++] = a[(v >> 6) & 63]; out[o++] = '=';
    }
    out[o] = '\0';
}

static void handle_file_get(sock_t s, const char *id, const char *line) {
    char path[600] = {0}, resp[FILE_GET_MAX * 4 / 3 + 400];
    unsigned char raw[FILE_GET_MAX];
    char b64[FILE_GET_MAX * 4 / 3 + 8];
    int off = 0, len = FILE_GET_MAX;
    long total = -1;
    size_t n = 0;
    FILE *f;
    if (!extract_str_unescaped_n(line, "path", path, sizeof(path))) {
        snprintf(resp, sizeof(resp), "{\"id\":\"%s\",\"type\":\"file_get_result\",\"ok\":false,\"error\":\"missing path\"}", id);
        send_line(s, resp);
        return;
    }
    extract_int(line, "offset", &off);
    extract_int(line, "len", &len);
    if (len <= 0 || len > FILE_GET_MAX) len = FILE_GET_MAX;
    f = fopen(path, "rb");
    if (!f) {
        snprintf(resp, sizeof(resp), "{\"id\":\"%s\",\"type\":\"file_get_result\",\"ok\":false,\"error\":\"cannot open file\"}", id);
        send_line(s, resp);
        return;
    }
    fseek(f, 0, SEEK_END); total = ftell(f);
    if (off >= 0 && off < total && fseek(f, off, SEEK_SET) == 0) n = fread(raw, 1, (size_t)len, f);
    fclose(f);
    b64_encode(raw, n, b64);
    snprintf(resp, sizeof(resp), "{\"id\":\"%s\",\"type\":\"file_get_result\",\"ok\":true,\"size\":%ld,\"offset\":%d,\"n\":%d,\"b64\":\"%s\"}", id, total, off, (int)n, b64);
    send_line(s, resp);
}

/* handle_exec -- run an arbitrary command on the client host and return its combined stdout+stderr
 * (card #599, HRIP-ctl: the founder explicitly chose full generic shell-exec over a narrow
 * HRIP-specific command set -- "Full generic shell-exec... reopens the exact 'no remote control'
 * door EDGE.GAME's NORTHSTAR says you closed" was presented and picked anyway). Unlike
 * editor_set/editor_get, there is deliberately NO path/extension allowlist here: this is a real,
 * wide capability, not a sandboxed one -- it rides the same encrypted (ML-KEM+XChaCha20-Poly1305)
 * transport as every other command, which is the only boundary this one has.
 * Output is capped at EXEC_OUT_MAX bytes (truncated, flagged, never silently dropped); there is no
 * timeout (a hung command hangs the client's single-threaded loop until the process exits or the
 * connection is killed and the client restarted) -- a known, named limitation, not an oversight. */
#define EXEC_OUT_MAX 48000
static void handle_exec(sock_t s, const char *id, const char *line) {
    char cmd[40000] = {0}; /* long enough for a base64 -EncodedCommand PowerShell script */
    FILE *p = NULL;
    unsigned char *raw = NULL;
    char *esc = NULL, *resp = NULL;
    size_t n = 0;
    int truncated = 0, exit_code = -1;
    if (!extract_str_unescaped_n(line, "cmd", cmd, sizeof(cmd))) {
        char e[200];
        snprintf(e, sizeof(e), "{\"id\":\"%s\",\"type\":\"exec_result\",\"ok\":false,\"error\":\"missing cmd\"}", id);
        send_line(s, e);
        return;
    }
#ifdef _WIN32
    {
        /* parens group the WHOLE command before redirecting -- "%s 2>&1" alone only redirects the
           last statement of a ";"/"&&" chain, silently dropping earlier stderr (found live, fixed
           here rather than shipped broken). cmd.exe supports ( ... ) grouping same as POSIX sh. */
        char full[40300];
        snprintf(full, sizeof(full), "cmd /C \"( %s ) 2>&1 <NUL\"", cmd); /* <NUL: a command that prompts (more, pause) gets EOF instead of hanging the single-threaded client */
        p = _popen(full, "rb");
    }
#else
    {
        char full[40300];
        snprintf(full, sizeof(full), "( %s ) 2>&1 </dev/null", cmd);
        p = popen(full, "r");
    }
#endif
    if (!p) {
        char e[300];
        snprintf(e, sizeof(e), "{\"id\":\"%s\",\"type\":\"exec_result\",\"ok\":false,\"error\":\"failed to start command\"}", id);
        send_line(s, e);
        return;
    }
    raw = (unsigned char *)malloc(EXEC_OUT_MAX + 1);
    if (raw) {
        n = fread(raw, 1, EXEC_OUT_MAX, p);
        if (n == EXEC_OUT_MAX) {
            /* drain the rest so the child doesn't block on a full pipe, but discard it */
            unsigned char junk[4096];
            size_t got;
            truncated = 1;
            while ((got = fread(junk, 1, sizeof(junk), p)) > 0) { (void)got; }
        }
    }
#ifdef _WIN32
    exit_code = p ? _pclose(p) : -1;
#else
    { int st = p ? pclose(p) : -1; exit_code = (st >= 0 && WIFEXITED(st)) ? WEXITSTATUS(st) : st; }
#endif
    if (raw) {
        esc = (char *)malloc(n * 6 + 8);
        resp = (char *)malloc(n * 6 + 300);
    }
    if (raw && esc && resp) {
        json_escape_bytes(raw, n, esc, n * 6 + 8);
        snprintf(resp, n * 6 + 300,
                 "{\"id\":\"%s\",\"type\":\"exec_result\",\"ok\":true,\"exit_code\":%d,\"truncated\":%s,\"output\":\"%s\"}",
                 id, exit_code, truncated ? "true" : "false", esc);
        send_line(s, resp);
    } else {
        char e[300];
        snprintf(e, sizeof(e), "{\"id\":\"%s\",\"type\":\"exec_result\",\"ok\":false,\"exit_code\":%d,\"error\":\"out of memory capturing output\"}", id, exit_code);
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

/* --- batteries-included browser login (EDGE-599-FOLLOWUP-2, founder real-time: "have it oauth
 * into IDUNA ... i need to run the client and then it opens the chrome into iduna so i can log
 * in"). Run with no token at all and the client opens the founder's own browser to IDUNA's
 * existing SSO login page, captures the JWT back via the classic desktop-OAuth loopback pattern
 * (a tiny local HTTP listener, since the SSO page hands the token back as a URL FRAGMENT --
 * never sent to a server by a browser -- so the served callback page's own JS reads
 * location.hash and POSTs it back same-origin), and uses that real IDUNA-issued JWT as the
 * token for both the cabinet and operator hello (relay_main.c's jwt_authorized). Real, named
 * limitation: a fixed local port (EDGE_LOGIN_CALLBACK_PORT overrides); if something else on the
 * machine already holds it, this fails and the operator still has to supply a token the old way. */
static void open_browser(const char *url) {
#ifdef _WIN32
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
#else
    char cmd[900];
    snprintf(cmd, sizeof(cmd), "xdg-open '%s' >/dev/null 2>&1 &", url);
    system(cmd); /* best-effort; the printed URL below is the real fallback */
#endif
    fprintf(stderr, "[edge_client] opening your browser to sign in to IDUNA.\n"
                     "[edge_client] if it doesn't open automatically, go to:\n%s\n", url);
}

static void url_decode(const char *in, size_t inlen, char *out, size_t outcap) {
    size_t i = 0, o = 0;
    while (i < inlen && o + 1 < outcap) {
        if (in[i] == '%' && i + 2 < inlen) {
            char hx[3] = { in[i + 1], in[i + 2], 0 };
            out[o++] = (char)strtol(hx, NULL, 16);
            i += 3;
        } else if (in[i] == '+') {
            out[o++] = ' ';
            i++;
        } else {
            out[o++] = in[i++];
        }
    }
    out[o] = '\0';
}

/* The page served at the loopback redirect_uri. IDUNA's own sso_login.go hands the token back as
 * "#sso_token=...&player_id=...&display_name=...", never reachable server-side directly -- this
 * JS is the only way a native app gets it out of the fragment. */
static const char *CALLBACK_PAGE =
    "<!doctype html><html><body style=\"font-family:sans-serif;padding:40px\">"
    "<p id=\"m\">Signing in...</p>"
    "<script>"
    "var h = location.hash.substring(1);"
    "if (h.indexOf('sso_token=') !== -1) {"
    "  fetch('/complete?' + h).then(function(){document.getElementById('m').textContent='Signed in -- you can close this window.';})"
    "  .catch(function(){document.getElementById('m').textContent='Something went wrong -- check edge_client.log.';});"
    "} else { document.getElementById('m').textContent = 'No token received from IDUNA.'; }"
    "</script></body></html>";

/* obtain_token_via_browser -- blocks until a real token arrives or the listener itself fails to
 * start. Returns 1 and fills token_out on success, 0 on failure (caller falls back to the old
 * manual-token usage message). */
static int obtain_token_via_browser(char *token_out, size_t cap) {
    int port = 51823;
    const char *port_env = getenv("EDGE_LOGIN_CALLBACK_PORT");
    if (port_env && *port_env) port = atoi(port_env);
    const char *iduna_base = getenv("EDGE_IDUNA_BASE_URL");
    if (!iduna_base || !*iduna_base) iduna_base = "https://iam.okemily.com";

    sock_t listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == INVALID_SOCKET) return 0;
    {
        int one = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* 127.0.0.1 only -- never exposed off-box */
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR || listen(listen_fd, 1) == SOCKET_ERROR) {
        fprintf(stderr, "[edge_client] could not open local port %d for sign-in (already in use?) -- "
                        "set EDGE_LOGIN_CALLBACK_PORT to another port, or pass a token directly.\n", port);
        CLOSESOCK(listen_fd);
        return 0;
    }

    char redirect_uri[128], login_url[768];
    snprintf(redirect_uri, sizeof(redirect_uri), "http://127.0.0.1:%d/callback", port);
    snprintf(login_url, sizeof(login_url), "%s/api/v1/auth/sso/login?logout=1&redirect_uri=%s", iduna_base, redirect_uri);
    open_browser(login_url);

    for (;;) {
        sock_t c = accept(listen_fd, NULL, NULL);
        if (c == INVALID_SOCKET) continue;
        char buf[8000] = {0};
        int n = recv(c, buf, sizeof(buf) - 1, 0);
        if (n <= 0) { CLOSESOCK(c); continue; }
        buf[n] = '\0';
        char path[512] = {0};
        sscanf(buf, "GET %511s", path); /* narrow, self-controlled: only ever "/callback" or "/complete?..." */
        char *q = strchr(path, '?');
        if (strncmp(path, "/complete", 9) == 0 && q) {
            char *t = strstr(q + 1, "sso_token=");
            if (t) {
                t += strlen("sso_token=");
                char *end = strchr(t, '&');
                size_t tl = end ? (size_t)(end - t) : strlen(t);
                url_decode(t, tl, token_out, cap);
                static const char *resp = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nok";
                send(c, resp, (int)strlen(resp), 0);
                CLOSESOCK(c);
                CLOSESOCK(listen_fd);
                return token_out[0] != '\0';
            }
            static const char *bad = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
            send(c, bad, (int)strlen(bad), 0);
            CLOSESOCK(c);
            continue;
        }
        {
            char resp[2400];
            int rn = snprintf(resp, sizeof(resp), "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n%s", CALLBACK_PAGE);
            send(c, resp, rn, 0);
        }
        CLOSESOCK(c);
    }
}

/* edge_exit -- double-clicked .exe windows vanish on exit, hiding the reason (founder: "the terminal
 * window closes after i sign in"). stdout/stderr are redirected to edge_client.log, so on Windows write
 * the final status to the real console and wait for Enter; elsewhere just return the code. */
static int g_authed = 0;
static int edge_exit(int code, const char *msg) {
    fprintf(stderr, "[edge_client] %s\n", msg);
#ifdef _WIN32
    {
        FILE *con = fopen("CONOUT$", "w");
        FILE *cin = fopen("CONIN$", "r");
        if (con) {
            fprintf(con, "\n[edge_client] %s\nDetails: edge_client.log (next to this exe). Press Enter to close.\n", msg);
            fflush(con);
        }
        if (cin) { int c; do { c = fgetc(cin); } while (c != '\n' && c != EOF); }
    }
#endif
    return code;
}

int main(int argc, char **argv) {
    /* File-backed logging, same pattern BIG_O's day/apps/client/src/main.c already established:
     * double-clicking a .exe gives you no console to read, and even running from a terminal the
     * window can close before a crash/early-exit message is visible. Redirect stdout+stderr to a
     * log file right next to the exe before anything else runs, so even an immediate exit (bad
     * args, connect failure, handshake failure) is captured. Overwrites each run (not appended) --
     * this is "what did THIS run do," not a growing history. Best-effort: if the redirect itself
     * fails (no write permission), both streams silently fall back to the original console. */
    FILE *log_stdout = freopen("edge_client.log", "w", stdout);
    FILE *log_stderr = freopen("edge_client.log", "a", stderr);
    (void)log_stdout;
    (void)log_stderr;
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (argc > 1 && strcmp(argv[1], "probe") == 0) { /* standalone: edge_client probe */
        char out[16384];
        if (probe_report(out, sizeof(out)) < 0) { fprintf(stderr, "probe output too large\n"); return 1; }
        printf("%s\n", out);
        return 0;
    }
    const char *host = argc > 1 ? argv[1] : (getenv("EDGE_RELAY_HOST") ? getenv("EDGE_RELAY_HOST") : "34.63.32.219"); /* the live relay behind tcp-edge */
    int port = argc > 2 ? atoi(argv[2]) : 8091;
    const char *token = argc > 3 ? argv[3] : getenv("EDGE_CLIENT_TOKEN");
    static char browser_token[2000];
#ifdef _WIN32
    WSADATA wsa; /* must run before obtain_token_via_browser's own socket() calls on Windows */
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    if (!token) {
        if (obtain_token_via_browser(browser_token, sizeof(browser_token))) {
            token = browser_token;
        } else {
            fprintf(stderr, "usage: %s <host> <port> <token>  (or run with no token to sign in via browser)\n", argv[0]);
            return 1;
        }
    }

    sock_t s = connect_to(host, port);
    if (s == INVALID_SOCKET) return edge_exit(1, "could not reach the relay (connect failed) -- check the host/port and your network");

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
        if (hs != 0) { char m[96]; snprintf(m, sizeof(m), "secure handshake with the relay failed (%d)", hs); return edge_exit(1, m); }
    }

    char hello[2300]; /* token can be a ~2000-char IDUNA JWT now, not just a short shared secret */
    {   /* EDGE_HOST=android on the tablet build; the PC console is "windows" (also what an old client implied) */
        const char *eh = getenv("EDGE_HOST");
        snprintf(hello, sizeof(hello), "{\"type\":\"hello\",\"token\":\"%s\",\"host\":\"%s\"}", token,
                 (eh && strcmp(eh, "android") == 0) ? "android" : "windows");
    }
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
                    fprintf(stderr, "[edge_client] authenticated\n"); g_authed = 1;
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
                } else if (strcmp(type, "exec") == 0 && id[0] != '\0') {
                    handle_exec(s, id, line);
                } else if (strcmp(type, "file_get") == 0 && id[0] != '\0') {
                    handle_file_get(s, id, line);
                } else if (strcmp(type, "serial_close") == 0 && id[0] != '\0') {
                    char resp[200];
                    if (g_sp) { serial_close(g_sp); g_sp = NULL; g_sp_len = 0; }
                    snprintf(resp, sizeof(resp), "{\"id\":\"%s\",\"type\":\"serial_close_result\",\"ok\":true}", id);
                    send_line(s, resp);
                    send_link(s);
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
    if (!g_authed) return edge_exit(2, "the relay closed the connection before accepting sign-in -- your IDUNA account probably lacks the edge.game.operator permission, or the token was rejected");
    return edge_exit(0, "disconnected from the relay");
}
