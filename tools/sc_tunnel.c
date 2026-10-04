/* tools/sc_tunnel.c -- generic TCP-over-secure-channel tunnel (K8S-GS-07: GFD pods -> IDUNA across
 * the internet). Same PARENA net/secure_channel transport EDGE.GAME's relay uses (ML-KEM-768 pinned
 * handshake, LZ4, XChaCha20-Poly1305), via common/sec_transport.c.
 *
 *   server (next to the service):  sc_tunnel server <listen-port> <target-host> <target-port> <keyfile>
 *   client (sidecar):              sc_tunnel client <listen-port> <server-host> <server-port> <pin-hex>
 *
 * One secure connection per accepted TCP connection (fork per connection). The client pin is the
 * server authentication; the server prints its pin on startup. No client auth at this layer: the
 * tunneled protocol (IDUNA's HTTP + JWT/agent secret) still authenticates itself.
 */
#define _GNU_SOURCE
#include "../common/sec_transport.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int dbg;
#define DBG(...) do { if (dbg) { fprintf(stderr, "sc_tunnel[%d]: ", (int)getpid()); fprintf(stderr, __VA_ARGS__); fputc(10, stderr); } } while (0)

static int dial(const char *host, const char *port) {
    struct addrinfo hints, *res, *p;
    int fd = -1;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;
    for (p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static int listen_on(int port) {
    struct sockaddr_in a;
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((unsigned short)port);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 128) != 0) { close(fd); return -1; }
    return fd;
}

/* Pump plain<->secure until either side closes. */
static void pump(SecConn *sc, int plain) {
    static unsigned char buf[SEC_MAX_PLAIN];
    for (;;) {
        struct pollfd pf[2];
        pf[0].fd = plain; pf[0].events = POLLIN; pf[0].revents = 0;
        pf[1].fd = sc->fd; pf[1].events = POLLIN; pf[1].revents = 0;
        /* a complete frame may already be buffered */
        for (;;) {
            int was = sc->established;
            long n = sec_next(sc, buf, sizeof(buf));
            if (n < 0) return;
            /* the handshake call returns 0 on completion but may leave a first data frame already
             * buffered behind the client's ct (it arrives in the same segment over a real network) */
            if (n == 0 && !was && sc->established) continue;
            if (n == 0) break;
            for (long off = 0; off < n;) {
                ssize_t w = write(plain, buf + off, (size_t)(n - off));
                if (w <= 0) return;
                off += w;
            }
        }
        DBG("poll (established=%d rawlen=%zu)", sc->established, sc->rawlen);
        if (poll(pf, 2, -1) < 0) { if (errno == EINTR) continue; return; }
        if (pf[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t r;
            if (!sc->established) { /* wait for handshake to finish before reading plaintext */ }
            else {
                r = read(plain, buf, sizeof(buf));
                if (r <= 0) return;
                if (sec_send(sc, buf, (size_t)r) != 0) return;
            }
        }
        if (pf[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (sec_fill(sc) <= 0) return;
        }
    }
}

typedef struct { int is_server; } Mode;

static void serve_one(int is_server, int accepted, const char *host, const char *port, const unsigned char *pin) {
    SecConn sc;
    int other;
    memset(&sc, 0, sizeof(sc));
    if (is_server) {
        /* accepted = secure side; host:port = plain target */
        other = dial(host, port);
        if (other < 0) { fprintf(stderr, "sc_tunnel: target %s:%s unreachable\n", host, port); close(accepted); return; }
        if (sec_server_begin(&sc, accepted) != 0) { close(accepted); close(other); return; }
        DBG("server begin ok, pumping");
        pump(&sc, other);
        DBG("pump ended");
        close(other); close(accepted);
    } else {
        SecPinPolicy pol;
        pol.pin = pin; pol.tofu_file = NULL; pol.hostport = NULL;
        other = dial(host, port);
        if (other < 0) { fprintf(stderr, "sc_tunnel: server %s:%s unreachable\n", host, port); close(accepted); return; }
        if (sec_client_handshake(&sc, other, sec_verify_pin_or_tofu, &pol) != 0) {
            fprintf(stderr, "sc_tunnel: handshake/pin check failed against %s:%s\n", host, port);
            close(other); close(accepted); return;
        }
        DBG("client handshake ok, pumping");
        pump(&sc, accepted);
        DBG("pump ended");
        close(other); close(accepted);
    }
}

int main(int argc, char **argv) {
    int is_server, lfd;
    unsigned char pin[SEC_PIN_BYTES];
    if (argc != 6 || (strcmp(argv[1], "server") && strcmp(argv[1], "client"))) {
        fprintf(stderr, "usage: %s server <listen-port> <target-host> <target-port> <keyfile>\n"
                        "       %s client <listen-port> <server-host> <server-port> <pin-hex>\n", argv[0], argv[0]);
        return 2;
    }
    is_server = strcmp(argv[1], "server") == 0;
    dbg = getenv("SC_DEBUG") != NULL;
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    if (is_server) {
        char hex[2 * SEC_PIN_BYTES + 1];
        if (sec_server_load_or_create_key(argv[5], pin) != 0) { fprintf(stderr, "sc_tunnel: key load failed\n"); return 1; }
        sec_hex(pin, SEC_PIN_BYTES, hex);
        fprintf(stderr, "sc_tunnel server: pin %s\n", hex);
    } else if (sec_unhex(argv[5], pin, SEC_PIN_BYTES) != 0) {
        fprintf(stderr, "sc_tunnel: pin must be 64 hex chars\n");
        return 2;
    }
    lfd = listen_on(atoi(argv[2]));
    if (lfd < 0) { perror("sc_tunnel: listen"); return 1; }
    for (;;) {
        int c = accept(lfd, NULL, NULL);
        if (c < 0) { if (errno == EINTR) continue; perror("accept"); return 1; }
        if (fork() == 0) {
            close(lfd);
            serve_one(is_server, c, argv[3], argv[4], pin);
            _exit(0);
        }
        close(c);
    }
}
