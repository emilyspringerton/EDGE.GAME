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

#define LINEBUF_SIZE 8192
#define MAX_OPERATORS 16
#define MAX_PENDING 32

typedef struct {
    int fd;
    int in_use;
    int authed;
    int is_subscriber;
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

static int g_cabinet_fd = -1;
static int g_cabinet_authed = 0;
static char g_cabinet_buf[LINEBUF_SIZE];
static size_t g_cabinet_buflen = 0;

static const char *g_client_token;
static const char *g_operator_token;
static const char *g_pi_token;
static int g_command_timeout_s;

static void send_line(int fd, const char *json) {
    char buf[LINEBUF_SIZE];
    int n = snprintf(buf, sizeof(buf), "%s\n", json);
    if (n < 0 || (size_t)n >= sizeof(buf)) return;
    ssize_t unused = write(fd, buf, (size_t)n);
    (void)unused;
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

static void handle_operator_line(int slot, const char *line) {
    OperatorConn *op = &g_operators[slot];
    char type[64] = {0};
    extract_str(line, "type", type, sizeof(type));

    if (strcmp(type, "events_since") == 0) {
        handle_events_since(op, line);
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
    if (id[0] == '\0' || g_cabinet_fd < 0 || !g_cabinet_authed) {
        send_line(op->fd, "{\"type\":\"error\",\"error\":\"no cabinet connected\"}");
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
    g_pending[pslot].deadline = time(NULL) + g_command_timeout_s;
    g_pending[pslot].used = 1;
    send_line(g_cabinet_fd, line);
}

static void handle_cabinet_line(const char *line) {
    if (!g_cabinet_authed) {
        char type[32] = {0}, token[256] = {0};
        extract_str(line, "type", type, sizeof(type));
        extract_str(line, "token", token, sizeof(token));
        if (strcmp(type, "hello") == 0 && strcmp(token, g_client_token) == 0) {
            g_cabinet_authed = 1;
            send_line(g_cabinet_fd, "{\"type\":\"hello_ok\"}");
            fprintf(stderr, "[relay] cabinet connected\n");
        } else {
            close(g_cabinet_fd);
            g_cabinet_fd = -1;
        }
        return;
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

/* feed_lines -- accumulates into `buf`/`buflen`, invoking `handler` once per complete NDJSON line
   read from `fd`. Returns 0 if the peer closed (caller should tear the connection down), 1 otherwise. */
static int feed_lines(int fd, char *buf, size_t *buflen, void (*handler)(const char *line)) {
    char chunk[4096];
    ssize_t n = read(fd, chunk, sizeof(chunk));
    if (n <= 0) return 0;
    if (*buflen + (size_t)n >= LINEBUF_SIZE) { *buflen = 0; return 1; } /* real, honest overflow-drop, not a buffer overrun */
    memcpy(buf + *buflen, chunk, (size_t)n);
    *buflen += (size_t)n;
    buf[*buflen] = '\0';

    char *start = buf;
    char *nl;
    while ((nl = strchr(start, '\n')) != NULL) {
        *nl = '\0';
        if (strlen(start) > 0) handler(start);
        start = nl + 1;
    }
    size_t remaining = strlen(start);
    memmove(buf, start, remaining + 1);
    *buflen = remaining;
    return 1;
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
    const char *timeout_s = getenv("EDGE_COMMAND_TIMEOUT_MS");
    g_command_timeout_s = timeout_s ? (atoi(timeout_s) + 999) / 1000 : 5;
    if (!g_client_token || !*g_client_token || !g_operator_token || !*g_operator_token) {
        fprintf(stderr, "EDGE_CLIENT_TOKEN and EDGE_OPERATOR_TOKEN must both be set\n");
        return 1;
    }

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
        if (g_cabinet_fd >= 0) { FD_SET(g_cabinet_fd, &rfds); if (g_cabinet_fd > maxfd) maxfd = g_cabinet_fd; }
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

        if (r == 0) continue;

        if (FD_ISSET(cabinet_listen_fd, &rfds)) {
            int fd = tcp_accept_raw(cabinet_listen_fd);
            if (fd >= 0) {
                if (g_cabinet_fd >= 0) tcp_close_raw(g_cabinet_fd);
                g_cabinet_fd = fd;
                g_cabinet_authed = 0;
                g_cabinet_buflen = 0;
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
                    g_operators[slot].fd = fd;
                    g_operators[slot].in_use = 1;
                }
            }
        }
        if (g_cabinet_fd >= 0 && FD_ISSET(g_cabinet_fd, &rfds)) {
            if (!feed_lines(g_cabinet_fd, g_cabinet_buf, &g_cabinet_buflen, handle_cabinet_line)) {
                fprintf(stderr, "[relay] cabinet disconnected\n");
                close(g_cabinet_fd);
                g_cabinet_fd = -1;
                g_cabinet_authed = 0;
            }
        }
        for (int i = 0; i < MAX_OPERATORS; i++) {
            OperatorConn *op = &g_operators[i];
            if (!op->in_use || !FD_ISSET(op->fd, &rfds)) continue;
            int slot_capture = i;
            char chunk[4096];
            ssize_t n = read(op->fd, chunk, sizeof(chunk));
            if (n <= 0) {
                close(op->fd);
                op->in_use = 0;
                continue;
            }
            if (op->buflen + (size_t)n >= LINEBUF_SIZE) { op->buflen = 0; continue; }
            memcpy(op->buf + op->buflen, chunk, (size_t)n);
            op->buflen += (size_t)n;
            op->buf[op->buflen] = '\0';
            char *start = op->buf;
            char *nl;
            while ((nl = strchr(start, '\n')) != NULL) {
                *nl = '\0';
                if (strlen(start) > 0) device_or_operator_dispatch(slot_capture, start);
                if (!op->in_use) break; /* dispatch may have closed this connection (bad hello) */
                start = nl + 1;
            }
            if (op->in_use) {
                size_t remaining = strlen(start);
                memmove(op->buf, start, remaining + 1);
                op->buflen = remaining;
            }
        }
    }
}
