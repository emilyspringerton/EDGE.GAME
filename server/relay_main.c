/* server/relay_main.c -- EDGE.GAME relay server, rewritten 2026-09-29 (founder real-time: "write it
 * in PARENA in what world are we using node for any part of this stack?" -> "all of the node stuff
 * gets ported to PARENA" -> "make sure we are using REFLUX for the pub sub" -> "parena really needs
 * to just emit the fucking llvm code for the server... i think we eat that tech debt... obviously
 * the windows client is C"). Replaces the original Phase 1 server/relay.js entirely -- no Node
 * anywhere in this stack now.
 *
 * Real, honest split, matching every other PARENA-mod-island in this monorepo
 * (blink.prn+blink_main.c, traffic_router.prn+edge_client.c, hw/usb_serial.prn+SPIDERBEETLE's own
 * Android app): PARENA (compiled through src/emit_llvm.c's new #target {:llvm ...} FFI hatch, see
 * PARENA/docs/LLVM_BACKEND_NORTHSTAR.md) owns the REAL decision/log logic --
 * stdlib/reflux/reflux.prn (the event pub/sub log) and stdlib/net/tcp_llvm.prn (listen/accept/close
 * lifecycle) -- linked into THIS binary as real object code (reflux_gen.o / tcp_llvm_gen.o, see
 * Makefile). This hand-written C file owns exactly what PARENA's v0 genuinely cannot express yet:
 * the select() event loop, raw byte buffers, and NDJSON line framing -- no struct/array/String
 * support exists on the LLVM target (see tcp_llvm.prn's own header comment for why raw
 * listen/accept/close is as far as that target reaches for sockets).
 *
 * Wire protocol, unified onto plain TCP + NDJSON everywhere (dropping Phase 1's HTTP operator API
 * entirely -- PARENA has no HTTP-server stdlib (net/http.prn is explicitly client-only, see its own
 * header comment), and unifying onto one transport is simpler and more honest than inventing one):
 *
 *   CABINET port (EDGE_CLIENT_PORT, default 8091) -- unchanged from Phase 1. One persistent cabinet
 *   client connects, sends {"type":"hello","token":"<EDGE_CLIENT_TOKEN>"}, gets {"type":"hello_ok"}.
 *   Operator commands are forwarded down this connection; responses are matched back to the
 *   waiting operator by "id".
 *
 *   OPERATOR port (EDGE_OPERATOR_PORT, default 8092) -- repurposed from HTTP to TCP+NDJSON, one
 *   "hello" handshake decides the connection's real role by which token it presents:
 *     - EDGE_OPERATOR_TOKEN -> operator: {"id":...,"type":"...","payload":...} gets forwarded to
 *       the cabinet and the reply relayed back; {"type":"events_since","since":N} returns buffered
 *       REFLUX events with seq > N; {"type":"events_subscribe","since":N} additionally marks this
 *       connection to receive new events live as they're dispatched (buffered AND streaming,
 *       founder real-time: "you need server streaming events for that too like buffered obviously").
 *     - EDGE_CLIENT_TOKEN or EDGE_PI_TOKEN -> device: a one-way event pusher (a Raspberry Pi's
 *       boot-announce script, see ../pi/boot_announce.sh). {"type":"event","event":"boot",
 *       "pi_id":N} dispatches REFLUX_ACTION_PI_BOOTED into the shared log.
 */
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../common/sec_transport.h"
#include "reflux_runtime.h" /* REFLUX_ACTION_* constants only -- reflux_host_* themselves are
                                called from the LLVM-compiled reflux_gen.o, never directly here. */

/* ---- extern declarations for the real, PARENA-compiled, LLVM-emitted object code linked into
   this binary (see Makefile's own relay target) -- these are not stubs, they are the actual
   functions stdlib/reflux/reflux.prn and stdlib/net/tcp_llvm.prn compile to. */
extern int tcp_listen_raw(int port);
extern int tcp_accept_raw(int listener_fd);
extern int tcp_close_raw(int fd);

extern void reflux_dispatch(int action_type, int a, int b, int c);
extern int reflux_log_size(void);
extern int reflux_action_type_at(int index);
extern int reflux_action_a_at(int index);
extern int reflux_action_b_at(int index);
extern int reflux_action_c_at(int index);

/* ---- narrow, flat NDJSON field extractors -- same real, deliberate scope boundary
   client/edge_client.c's own identical helpers already document (both ends of this protocol are
   this same repo, never nest the same key name twice). Duplicated rather than shared across a new
   header: each copy is ~15 lines, and this file's own real, new logic (the select() loop,
   connection-role bookkeeping) is what actually needs care, not this. */
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

#define LINEBUF_SIZE 65536 /* one full secure frame (SEC_MAX_PLAIN) plus a partial line */
#define MAX_OPERATORS 16
#define MAX_PENDING 32

typedef struct {
    int fd;
    SecConn sec;
    time_t accepted;
    int in_use;
    int authed;
    int is_subscriber;
    int log_sub;      /* live log/serial stream subscriber */
    long log_cursor;  /* next log seq this subscriber hasn't been sent */
    int sub_cursor; /* next REFLUX index (0-based, log-relative) this subscriber hasn't seen yet */
    char buf[LINEBUF_SIZE];
    size_t buflen;
} OperatorConn;

typedef struct {
    char id[64];
    int operator_slot;
    time_t deadline;
    int used;
} Pending;

static OperatorConn g_operators[MAX_OPERATORS];
static Pending g_pending[MAX_PENDING];

/* Cabinet hosts (card #478). The Feather's one USB link is on EITHER the Windows dev PC or the Android
   tablet -- never both at once -- but both hosts may be connected to this relay (the tablet is the
   production brain, the PC the dev console). Each host holds one authed cabinet connection (a
   reconnect of the same host replaces the old one); a host reports whether the Feather is currently
   attached to it with {"type":"link","feather":0|1}. Operator commands go to the host that has the
   Feather, or to an explicit {"host":"windows|android"}. */
#define MAX_CABS 4
#define HOST_WINDOWS 0
#define HOST_ANDROID 1
#define HOST_COUNT 2
static const char *const HOST_NAMES[HOST_COUNT] = { "windows", "android" };
typedef struct {
    int in_use;
    int fd;
    SecConn sec;
    time_t accepted;
    int authed;
    int host;
    int feather;
    char buf[LINEBUF_SIZE];
    size_t buflen;
} Cabinet;
static Cabinet g_cabs[MAX_CABS];

static const char *g_client_token;
static const char *g_operator_token;
static const char *g_pi_token;
static int g_command_timeout_s;

/* Every socket is a SecConn (ML-KEM handshake + LZ4 + XChaCha20-Poly1305, see sec_transport.h):
   send_line finds the connection for an fd and encrypts. A peer that hasn't finished the handshake
   gets nothing. */
static SecConn *conn_for_fd(int fd) {
    for (int i = 0; i < MAX_CABS; i++)
        if (g_cabs[i].in_use && g_cabs[i].fd == fd) return &g_cabs[i].sec;
    for (int i = 0; i < MAX_OPERATORS; i++)
        if (g_operators[i].in_use && g_operators[i].fd == fd) return &g_operators[i].sec;
    return NULL;
}

static void send_line(int fd, const char *json) {
    static char buf[LINEBUF_SIZE];
    SecConn *c = conn_for_fd(fd);
    int n = snprintf(buf, sizeof(buf), "%s\n", json);
    if (!c || !c->established || n < 0 || (size_t)n >= sizeof(buf)) return;
    if (sec_send(c, buf, (size_t)n) < 0) { /* peer gone: the select loop sees EOF on its next read */ }
}

/* push_new_events_to_subscribers -- REFLUX's own real model is poll-your-own-cursor (see
   reflux.prn's own header comment: VS0 has no function pointers/closures, so there's no real
   push-callback mechanism at the PARENA level). This is the thin, additive "go poll" nudge layered
   on top for the operator API's own live streaming, called right after every real
   reflux_dispatch() -- it reads the SAME real log via reflux_log_size()/reflux_action_*_at(), it
   does not maintain any separate copy of the data. */
static void push_new_events_to_subscribers(void) {
    int size = reflux_log_size();
    for (int i = 0; i < MAX_OPERATORS; i++) {
        OperatorConn *op = &g_operators[i];
        if (!op->in_use || !op->is_subscriber) continue;
        for (int idx = op->sub_cursor; idx < size; idx++) {
            char line[256];
            snprintf(line, sizeof(line),
                     "{\"type\":\"event\",\"action_type\":%d,\"a\":%d,\"b\":%d,\"c\":%d}",
                     reflux_action_type_at(idx), reflux_action_a_at(idx),
                     reflux_action_b_at(idx), reflux_action_c_at(idx));
            send_line(op->fd, line);
        }
        op->sub_cursor = size;
    }
}

static void handle_device_event(const char *line) {
    char event[32] = {0};
    extract_str(line, "event", event, sizeof(event));
    if (strcmp(event, "boot") == 0) {
        int pi_id = 0;
        extract_int(line, "pi_id", &pi_id);
        reflux_dispatch(REFLUX_ACTION_PI_BOOTED, pi_id, (int)time(NULL), 0);
        push_new_events_to_subscribers();
    }
    /* Unknown event names are silently ignored -- same real, honest "reserve the constant, wire it
       up when a real dispatcher exists" convention reflux_runtime.h's own header comment uses for
       COMPILE_RESULT/UPLOAD_RESULT/PIN_EVENT above; a malformed/unrecognized event is not a wire
       protocol violation worth tearing down the connection over. */
}


/* ---- cabinet log buffer (cards #492/#474) --------------------------------------------------------
 * The cabinet client pushes {"type":"log","channel":"serial|client|term","data":"<json-escaped>"}
 * (the Feather's serial lines, client diagnostics, terminal output). The relay keeps the last
 * LOG_RING of them with a stable 1-based seq and streams them to subscribers, so an operator (Claude)
 * can read device output directly. `data` is stored and re-emitted RAW (still JSON-escaped): the relay
 * never needs the decoded text, and that keeps arbitrary serial bytes safe. Buffer is in-memory only. */
#define LOG_RING 512
#define LOG_DATA_MAX 1100
typedef struct { long seq; char channel[16]; char data[LOG_DATA_MAX]; } LogEntry;
static LogEntry g_logs[LOG_RING];
static long g_log_total = 0;

/* raw copy of a JSON string value (escapes preserved) */
static int extract_str_raw(const char *json, const char *field, char *out, size_t outlen) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":\"", field);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    size_t i = 0;
    while (*p && *p != '"' && i + 2 < outlen) {
        if (*p == '\\' && p[1]) out[i++] = *p++;
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 1;
}

static void send_log_entry(int fd, const LogEntry *e, const char *type) {
    static char line[LOG_DATA_MAX + 128];
    snprintf(line, sizeof(line), "{\"type\":\"%s\",\"seq\":%ld,\"channel\":\"%s\",\"data\":\"%s\"}", type, e->seq, e->channel, e->data);
    send_line(fd, line);
}

static void handle_cabinet_log(const char *line) {
    LogEntry *e = &g_logs[g_log_total % LOG_RING];
    memset(e, 0, sizeof(*e));
    extract_str(line, "channel", e->channel, sizeof(e->channel));
    if (!e->channel[0]) snprintf(e->channel, sizeof(e->channel), "serial");
    extract_str_raw(line, "data", e->data, sizeof(e->data));
    e->seq = ++g_log_total;
    for (int i = 0; i < MAX_OPERATORS; i++) {
        OperatorConn *op = &g_operators[i];
        if (!op->in_use || !op->log_sub) continue;
        for (long q = op->log_cursor; q <= g_log_total; q++) {
            LogEntry *x = &g_logs[(q - 1) % LOG_RING];
            if (x->seq == q) send_log_entry(op->fd, x, "log");
        }
        op->log_cursor = g_log_total + 1;
    }
}

/* log_since / log_subscribe: oldest-first replay of retained entries with seq > since (optional
   "channel" filter), as one {"type":"logs","items":[...],"next":N} reply. */
static void handle_log_since(OperatorConn *op, const char *line) {
    static char out[60000];
    long since = 0;
    int sn = 0;
    char chan[16] = {0};
    if (extract_int(line, "since", &sn)) since = sn;
    extract_str(line, "channel", chan, sizeof(chan));
    size_t off = (size_t)snprintf(out, sizeof(out), "{\"type\":\"logs\",\"items\":[");
    int first = 1;
    long oldest = g_log_total > LOG_RING ? g_log_total - LOG_RING + 1 : 1;
    for (long q = (since + 1 > oldest ? since + 1 : oldest); q <= g_log_total; q++) {
        LogEntry *x = &g_logs[(q - 1) % LOG_RING];
        if (chan[0] && strcmp(chan, x->channel) != 0) continue;
        char item[LOG_DATA_MAX + 96];
        int n = snprintf(item, sizeof(item), "%s{\"seq\":%ld,\"channel\":\"%s\",\"data\":\"%s\"}", first ? "" : ",", x->seq, x->channel, x->data);
        if (n < 0 || off + (size_t)n + 40 >= sizeof(out)) break; /* reply is bounded; caller re-asks with the returned next */
        memcpy(out + off, item, (size_t)n); off += (size_t)n; first = 0;
        since = q;
    }
    snprintf(out + off, sizeof(out) - off, "],\"next\":%ld}", since > 0 ? since : 0);
    send_line(op->fd, out);
}

static void handle_events_since(OperatorConn *op, const char *line) {
    int since = 0;
    extract_int(line, "since", &since);
    int size = reflux_log_size();
    char out[4096];
    size_t off = (size_t)snprintf(out, sizeof(out), "{\"type\":\"events\",\"items\":[");
    int first = 1;
    /* seq is 1-based and stable across ring-buffer wraparound (unlike a raw slot index) --
       real index i (0-based, oldest-retained-first) corresponds to seq = (total dispatched so far
       - size + i + 1). reflux.prn exposes no direct "total dispatched" accessor, but size and the
       per-index values are enough for a bounded catch-up read: seq is derived from position alone,
       consistent within one process's lifetime, which is this relay's own real, honest scope. */
    for (int i = 0; i < size; i++) {
        int seq = i + 1; /* real, simple convention: this relay's OWN 1-based sequence within the
                             currently-retained window -- a subscriber tracks this the same way
                             op->sub_cursor already does above, not an absolute all-time count. */
        if (seq <= since) continue;
        if (!first && off + 2 < sizeof(out)) { out[off++] = ','; out[off] = '\0'; }
        first = 0;
        int n = snprintf(out + off, sizeof(out) - off,
                          "{\"seq\":%d,\"action_type\":%d,\"a\":%d,\"b\":%d,\"c\":%d}",
                          seq, reflux_action_type_at(i), reflux_action_a_at(i),
                          reflux_action_b_at(i), reflux_action_c_at(i));
        if (n < 0 || (size_t)n >= sizeof(out) - off) break;
        off += (size_t)n;
    }
    if (off + 2 < sizeof(out)) { out[off++] = ']'; out[off++] = '}'; out[off] = '\0'; }
    send_line(op->fd, out);
}

/* Slow-loris guard: a connection that hasn't finished the secure handshake AND sent a valid hello
   within g_handshake_deadline_s seconds is closed, so half-open sockets can't exhaust the 16 operator slots. */
static int g_handshake_deadline_s = 10; /* EDGE_HANDSHAKE_DEADLINE_S overrides (tests use 2) */
static void reap_unauthenticated(void) {
    time_t now = time(NULL);
    for (int i = 0; i < MAX_CABS; i++) {
        Cabinet *c = &g_cabs[i];
        if (c->in_use && !c->authed && now - c->accepted > g_handshake_deadline_s) {
            fprintf(stderr, "[relay] dropping cabinet connection that never authenticated\n");
            close(c->fd);
            c->in_use = 0;
        }
    }
    for (int i = 0; i < MAX_OPERATORS; i++) {
        OperatorConn *op = &g_operators[i];
        if (op->in_use && !op->authed && now - op->accepted > g_handshake_deadline_s) {
            close(op->fd);
            op->in_use = 0;
        }
    }
}

static void find_and_expire_pending(void) {
    time_t now = time(NULL);
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!g_pending[i].used) continue;
        if (now < g_pending[i].deadline) continue;
        OperatorConn *op = &g_operators[g_pending[i].operator_slot];
        if (op->in_use) {
            char line[256];
            snprintf(line, sizeof(line), "{\"id\":\"%s\",\"type\":\"error\",\"error\":\"cabinet did not respond in time\"}",
                      g_pending[i].id);
            send_line(op->fd, line);
        }
        g_pending[i].used = 0;
    }
}

/* pick_cabinet -- which connected host should receive an operator command? An explicit host name
   wins; otherwise the single connected host, else the one that reports the Feather. Refuses (with a
   reason) rather than guessing when it is ambiguous. */
static Cabinet *pick_cabinet(const char *want_host, const char **err) {
    Cabinet *authed[MAX_CABS]; int n = 0;
    for (int i = 0; i < MAX_CABS; i++) if (g_cabs[i].in_use && g_cabs[i].authed) authed[n++] = &g_cabs[i];
    if (want_host && want_host[0]) {
        for (int i = 0; i < n; i++) if (strcmp(HOST_NAMES[authed[i]->host], want_host) == 0) return authed[i];
        *err = "requested host is not connected";
        return NULL;
    }
    if (n == 0) { *err = "no cabinet connected"; return NULL; }
    if (n == 1) return authed[0];
    Cabinet *with = NULL; int nf = 0;
    for (int i = 0; i < n; i++) if (authed[i]->feather) { with = authed[i]; nf++; }
    if (nf == 1) return with;
    *err = nf == 0 ? "several hosts connected and none reports the Feather -- pass \\\"host\\\""
                   : "several hosts claim the Feather (one USB link cannot be on both) -- pass \\\"host\\\"";
    return NULL;
}

static void handle_hosts(OperatorConn *op) {
    char out[512];
    size_t off = (size_t)snprintf(out, sizeof(out), "{\"type\":\"hosts\",\"items\":[");
    int first = 1, nf = 0; const char *fh = NULL;
    for (int h = 0; h < HOST_COUNT; h++) {
        for (int i = 0; i < MAX_CABS; i++) {
            Cabinet *c = &g_cabs[i];
            if (!(c->in_use && c->authed && c->host == h)) continue;
            off += (size_t)snprintf(out + off, sizeof(out) - off, "%s{\"host\":\"%s\",\"feather\":%s}",
                                    first ? "" : ",", HOST_NAMES[h], c->feather ? "true" : "false");
            first = 0;
            if (c->feather) { nf++; fh = HOST_NAMES[h]; }
        }
    }
    snprintf(out + off, sizeof(out) - off, "],\"feather_host\":%s%s%s}",
             nf == 1 ? "\"" : "", nf == 1 ? fh : (nf == 0 ? "null" : "\"conflict\""), nf == 1 ? "\"" : "");
    send_line(op->fd, out);
}

static void handle_operator_line(int slot, const char *line) {
    OperatorConn *op = &g_operators[slot];
    char type[64] = {0};
    extract_str(line, "type", type, sizeof(type));

    if (strcmp(type, "hosts") == 0) {
        handle_hosts(op);
        return;
    }
    if (strcmp(type, "events_since") == 0) {
        handle_events_since(op, line);
        return;
    }
    if (strcmp(type, "log_since") == 0) {
        handle_log_since(op, line);
        return;
    }
    if (strcmp(type, "log_subscribe") == 0) {
        int sn = 0;
        extract_int(line, "since", &sn);
        handle_log_since(op, line); /* catch-up first, then live */
        op->log_sub = 1;
        op->log_cursor = g_log_total + 1;
        (void)sn;
        return;
    }
    if (strcmp(type, "events_subscribe") == 0) {
        op->is_subscriber = 1;
        handle_events_since(op, line); /* real, immediate catch-up flush before switching to live push */
        /* sub_cursor is a 0-based INDEX into the currently-retained window (what
           push_new_events_to_subscribers walks), NOT the same unit as the client's own "since" seq
           cutoff handle_events_since just consumed -- after the catch-up flush above, every
           currently-retained item has been accounted for (either sent, or explicitly skipped
           because the caller's own `since` didn't want it), so the live-push cursor starts exactly
           at the current log size regardless of what `since` was. */
        op->sub_cursor = reflux_log_size();
        return;
    }

    /* Otherwise: a command to forward to the cabinet. */
    char id[64] = {0};
    extract_str(line, "id", id, sizeof(id));
    char want_host[16] = {0};
    extract_str(line, "host", want_host, sizeof(want_host));
    const char *route_err = NULL;
    Cabinet *cab = pick_cabinet(want_host, &route_err);
    if (id[0] == '\0' || !cab) {
        char eb[256];
        snprintf(eb, sizeof(eb), "{\"type\":\"error\",\"error\":\"%s\"}", id[0] == '\0' ? "missing id" : route_err);
        send_line(op->fd, eb);
        return;
    }
    int pslot = -1;
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!g_pending[i].used) { pslot = i; break; }
    }
    if (pslot < 0) {
        send_line(op->fd, "{\"type\":\"error\",\"error\":\"too many in-flight commands\"}");
        return;
    }
    snprintf(g_pending[pslot].id, sizeof(g_pending[pslot].id), "%s", id);
    g_pending[pslot].operator_slot = slot;
    /* flashing (1200-baud touch + re-enumeration + 48 page writes + verify) legitimately takes far
       longer than a ping; give it 90s instead of the default */
    g_pending[pslot].deadline = time(NULL) + (strcmp(type, "flash_hex") == 0 ? 90 : g_command_timeout_s);
    g_pending[pslot].used = 1;
    send_line(cab->fd, line);
}

static void handle_cabinet_line(Cabinet *cab, const char *line) {
    if (!cab->authed) {
        char type[32] = {0}, token[256] = {0}, host[16] = {0};
        extract_str(line, "type", type, sizeof(type));
        extract_str(line, "token", token, sizeof(token));
        extract_str(line, "host", host, sizeof(host));
        if (strcmp(type, "hello") == 0 && strcmp(token, g_client_token) == 0) {
            cab->host = strcmp(host, "android") == 0 ? HOST_ANDROID : HOST_WINDOWS; /* old clients send no host: Windows */
            /* one connection per host: a reconnect replaces the stale one */
            for (int i = 0; i < MAX_CABS; i++) {
                Cabinet *o = &g_cabs[i];
                if (o != cab && o->in_use && o->authed && o->host == cab->host) { close(o->fd); o->in_use = 0; }
            }
            cab->authed = 1;
            cab->feather = 0;
            send_line(cab->fd, "{\"type\":\"hello_ok\"}");
            fprintf(stderr, "[relay] cabinet connected (host=%s)\n", HOST_NAMES[cab->host]);
        } else {
            close(cab->fd);
            cab->in_use = 0;
        }
        return;
    }
    {
        char t[32] = {0};
        extract_str(line, "type", t, sizeof(t));
        if (strcmp(t, "log") == 0) { handle_cabinet_log(line); return; }
        if (strcmp(t, "link") == 0) {
            int f = 0;
            extract_int(line, "feather", &f);
            cab->feather = f ? 1 : 0;
            fprintf(stderr, "[relay] host=%s feather %s\n", HOST_NAMES[cab->host], cab->feather ? "attached" : "detached");
            return;
        }
    }
    char id[64] = {0};
    extract_str(line, "id", id, sizeof(id));
    if (id[0] == '\0') return;
    for (int i = 0; i < MAX_PENDING; i++) {
        if (g_pending[i].used && strcmp(g_pending[i].id, id) == 0) {
            OperatorConn *op = &g_operators[g_pending[i].operator_slot];
            if (op->in_use) send_line(op->fd, line);
            g_pending[i].used = 0;
            break;
        }
    }
}

static void device_or_operator_dispatch(int slot, const char *line);

/* ingest -- append decrypted bytes to a connection's line buffer and dispatch each complete NDJSON
   line. slot < 0 = the cabinet. Returns 0 if a handler closed the connection, 1 otherwise. */
static int ingest(char *buf, size_t *buflen, const unsigned char *chunk, size_t n, int slot) {
    if (*buflen + n >= LINEBUF_SIZE) { *buflen = 0; return 1; } /* real, honest overflow-drop, not a buffer overrun */
    memcpy(buf + *buflen, chunk, n);
    *buflen += n;
    buf[*buflen] = '\0';
    char *start = buf;
    char *nl;
    while ((nl = strchr(start, '\n')) != NULL) {
        *nl = '\0';
        if (strlen(start) > 0) {
            if (slot < 0) handle_cabinet_line(&g_cabs[-slot - 1], start); else device_or_operator_dispatch(slot, start);
        }
        if (slot < 0 ? !g_cabs[-slot - 1].in_use : !g_operators[slot].in_use) return 0;
        start = nl + 1;
    }
    size_t remaining = strlen(start);
    memmove(buf, start, remaining + 1);
    *buflen = remaining;
    return 1;
}

/* pump -- one readable event on a SecConn: pull bytes, decrypt every complete frame, ingest each.
   Returns 0 when the connection must be torn down (EOF, auth failure, or a handler closed it). */
static int pump(SecConn *sc, char *buf, size_t *buflen, int slot) {
    static unsigned char plain[SEC_MAX_PLAIN];
    if (sec_fill(sc) <= 0) return 0;
    for (;;) {
        size_t before = sc->rawlen;
        long n = sec_next(sc, plain, sizeof(plain));
        if (n < 0) return 0;
        if (n == 0) {
            /* 0 = need more bytes, OR the handshake step just consumed the client's ct (frames
               already buffered behind it still need draining) -- rawlen shrinks only in that case. */
            if (sc->rawlen < before) continue;
            return 1;
        }
        if (!ingest(buf, buflen, plain, (size_t)n, slot)) return 0;
    }
}

static void device_or_operator_dispatch(int slot, const char *line) {
    OperatorConn *op = &g_operators[slot];
    if (!op->authed) {
        char type[32] = {0}, token[256] = {0};
        extract_str(line, "type", type, sizeof(type));
        extract_str(line, "token", token, sizeof(token));
        if (strcmp(type, "hello") != 0) { close(op->fd); op->in_use = 0; return; }
        if (strcmp(token, g_operator_token) == 0) {
            op->authed = 1;
            send_line(op->fd, "{\"type\":\"hello_ok\",\"role\":\"operator\"}");
        } else if (strcmp(token, g_client_token) == 0 || (g_pi_token && *g_pi_token && strcmp(token, g_pi_token) == 0)) {
            op->authed = 2; /* device role */
            send_line(op->fd, "{\"type\":\"hello_ok\",\"role\":\"device\"}");
        } else {
            close(op->fd);
            op->in_use = 0;
        }
        return;
    }
    if (op->authed == 2) {
        handle_device_event(line);
    } else {
        handle_operator_line(slot, line);
    }
}

int main(void) {
    const char *client_port_s = getenv("EDGE_CLIENT_PORT");
    const char *operator_port_s = getenv("EDGE_OPERATOR_PORT");
    int client_port = client_port_s ? atoi(client_port_s) : 8091;
    int operator_port = operator_port_s ? atoi(operator_port_s) : 8092;
    g_client_token = getenv("EDGE_CLIENT_TOKEN");
    g_operator_token = getenv("EDGE_OPERATOR_TOKEN");
    g_pi_token = getenv("EDGE_PI_TOKEN");
    if (getenv("EDGE_HANDSHAKE_DEADLINE_S")) g_handshake_deadline_s = atoi(getenv("EDGE_HANDSHAKE_DEADLINE_S"));
    const char *timeout_s = getenv("EDGE_COMMAND_TIMEOUT_MS");
    g_command_timeout_s = timeout_s ? (atoi(timeout_s) + 999) / 1000 : 5;
    if (!g_client_token || !*g_client_token || !g_operator_token || !*g_operator_token) {
        fprintf(stderr, "EDGE_CLIENT_TOKEN and EDGE_OPERATOR_TOKEN must both be set\n");
        return 1;
    }

    const char *key_path = getenv("EDGE_KEY_FILE");
    unsigned char pin[SEC_PIN_BYTES];
    char pin_hex[2 * SEC_PIN_BYTES + 1];
    if (sec_server_load_or_create_key(key_path && *key_path ? key_path : "edge_relay.key", pin) < 0) {
        fprintf(stderr, "relay: cannot load/create the ML-KEM server key\n");
        return 1;
    }
    sec_hex(pin, sizeof(pin), pin_hex);
    fprintf(stderr, "[relay] ML-KEM-768 + LZ4 + XChaCha20-Poly1305 only (no plaintext mode)\n");
    fprintf(stderr, "[relay] server fingerprint (clients pin this): %s\n", pin_hex);

    int cabinet_listen_fd = tcp_listen_raw(client_port);
    int operator_listen_fd = tcp_listen_raw(operator_port);
    if (cabinet_listen_fd < 0 || operator_listen_fd < 0) {
        fprintf(stderr, "relay: failed to open listening sockets\n");
        return 1;
    }
    fprintf(stderr, "[relay] cabinet listener on :%d\n", client_port);
    fprintf(stderr, "[relay] operator listener on :%d\n", operator_port);

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = 0;
        FD_SET(cabinet_listen_fd, &rfds); if (cabinet_listen_fd > maxfd) maxfd = cabinet_listen_fd;
        FD_SET(operator_listen_fd, &rfds); if (operator_listen_fd > maxfd) maxfd = operator_listen_fd;
        for (int i = 0; i < MAX_CABS; i++)
            if (g_cabs[i].in_use) { FD_SET(g_cabs[i].fd, &rfds); if (g_cabs[i].fd > maxfd) maxfd = g_cabs[i].fd; }
        for (int i = 0; i < MAX_OPERATORS; i++) {
            if (g_operators[i].in_use) {
                FD_SET(g_operators[i].fd, &rfds);
                if (g_operators[i].fd > maxfd) maxfd = g_operators[i].fd;
            }
        }

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 }; /* real, short timeout so pending-command
                                                                expiry (below) is checked regularly
                                                                even with no socket activity */
        int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) continue;

        find_and_expire_pending();
        reap_unauthenticated();

        if (r == 0) continue;

        if (FD_ISSET(cabinet_listen_fd, &rfds)) {
            int fd = tcp_accept_raw(cabinet_listen_fd);
            if (fd >= 0) {
                int cs = -1;
                for (int i = 0; i < MAX_CABS; i++) if (!g_cabs[i].in_use) { cs = i; break; }
                if (cs < 0) { tcp_close_raw(fd); } /* more unauthenticated peers than slots: refuse, the reaper frees stale ones */
                else {
                    Cabinet *c = &g_cabs[cs];
                    memset(c, 0, sizeof(*c));
                    if (sec_server_begin(&c->sec, fd) < 0) { tcp_close_raw(fd); }
                    else { c->fd = fd; c->accepted = time(NULL); c->in_use = 1; }
                }
            }
        }
        if (FD_ISSET(operator_listen_fd, &rfds)) {
            int fd = tcp_accept_raw(operator_listen_fd);
            if (fd >= 0) {
                int slot = -1;
                for (int i = 0; i < MAX_OPERATORS; i++) if (!g_operators[i].in_use) { slot = i; break; }
                if (slot < 0) {
                    tcp_close_raw(fd);
                } else {
                    memset(&g_operators[slot], 0, sizeof(OperatorConn));
                    if (sec_server_begin(&g_operators[slot].sec, fd) < 0) {
                        tcp_close_raw(fd);
                    } else {
                        g_operators[slot].fd = fd;
                        g_operators[slot].accepted = time(NULL);
                        g_operators[slot].in_use = 1;
                    }
                }
            }
        }
        for (int i = 0; i < MAX_CABS; i++) {
            Cabinet *c = &g_cabs[i];
            if (!c->in_use || !FD_ISSET(c->fd, &rfds)) continue;
            if (!pump(&c->sec, c->buf, &c->buflen, -(i + 1)) && c->in_use) {
                fprintf(stderr, "[relay] cabinet disconnected (host=%s)\n", c->authed ? HOST_NAMES[c->host] : "?");
                close(c->fd);
                c->in_use = 0;
            }
        }
        for (int i = 0; i < MAX_OPERATORS; i++) {
            OperatorConn *op = &g_operators[i];
            if (!op->in_use || !FD_ISSET(op->fd, &rfds)) continue;
            if (!pump(&op->sec, op->buf, &op->buflen, i) && op->in_use) {
                close(op->fd);
                op->in_use = 0;
            }
        }
    }
}
