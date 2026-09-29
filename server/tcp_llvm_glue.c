/* server/tcp_llvm_glue.c -- real, externally-linked implementations of the three C functions
 * stdlib/net/tcp_llvm.prn's own :llvm FFI bodies `declare` and `call` (tcp_listen_impl/
 * tcp_accept_impl/tcp_close_impl).
 *
 * NOT a #include of PARENA/runtime/parena_runtime.h, even though that header already defines
 * functions with these exact names and bodies (net/tcp.prn's own :c FFI target calls the same
 * three) -- those are declared `static inline`, i.e. INTERNAL linkage: fine for a single C
 * translation unit that both defines and calls them (exactly how the :c target already uses them),
 * but invisible to a SEPARATELY-compiled object file's `declare i32 @tcp_listen_impl(i32)` +
 * `call`, which needs a real, externally-linked symbol to resolve against at link time. Rather than
 * pull in the rest of that large, syscall-heavy shared header (sockets, ptys, mmap, SPI/I2C device
 * files -- far more than this relay needs) just to get non-static copies, these three bodies are
 * duplicated here verbatim (they're each 3-10 lines) with real, external linkage instead.
 */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

int tcp_listen_impl(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) { close(fd); return -1; }
    if (listen(fd, 16) < 0) { close(fd); return -1; }
    return fd;
}

int tcp_accept_impl(int listener_fd) {
    return accept(listener_fd, NULL, NULL);
}

int tcp_close_impl(int fd) {
    return close(fd) == 0 ? 0 : -1;
}
