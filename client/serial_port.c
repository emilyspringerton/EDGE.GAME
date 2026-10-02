/* client/serial_port.c -- see serial_port.h. */
#define _DEFAULT_SOURCE 1 /* cfmakeraw, usleep under -std=c99 */
#include "serial_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>

struct SerialPort { HANDLE h; };

SerialPort *serial_open(const char *port, int baud, char *errbuf, size_t errlen) {
    char path[64];
    SerialPort *sp;
    DCB dcb;
    COMMTIMEOUTS to;
    /* COM10+ need the \\.\ prefix; it is harmless for COM1-9, so always add it unless present. */
    if (strncmp(port, "\\\\.\\", 4) == 0) snprintf(path, sizeof(path), "%s", port);
    else snprintf(path, sizeof(path), "\\\\.\\%s", port);
    sp = (SerialPort *)calloc(1, sizeof(*sp));
    if (!sp) return NULL;
    sp->h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (sp->h == INVALID_HANDLE_VALUE) {
        snprintf(errbuf, errlen, "CreateFile %s failed (Win32 error %lu) -- is another program (Arduino IDE, PuTTY) holding the port?", path, GetLastError());
        free(sp); return NULL;
    }
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(sp->h, &dcb)) { snprintf(errbuf, errlen, "GetCommState failed (%lu)", GetLastError()); CloseHandle(sp->h); free(sp); return NULL; }
    dcb.BaudRate = (DWORD)baud; dcb.ByteSize = 8; dcb.Parity = NOPARITY; dcb.StopBits = ONESTOPBIT;
    dcb.fOutxCtsFlow = FALSE; dcb.fOutxDsrFlow = FALSE; dcb.fDtrControl = DTR_CONTROL_ENABLE; dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutX = FALSE; dcb.fInX = FALSE;
    if (!SetCommState(sp->h, &dcb)) { snprintf(errbuf, errlen, "SetCommState failed (%lu)", GetLastError()); CloseHandle(sp->h); free(sp); return NULL; }
    /* ReadFile returns immediately with whatever is buffered (MAXDWORD interval, 0 multiplier/constant). */
    memset(&to, 0, sizeof(to));
    to.ReadIntervalTimeout = MAXDWORD;
    to.WriteTotalTimeoutConstant = 1000;
    SetCommTimeouts(sp->h, &to);
    return sp;
}

int serial_read(SerialPort *sp, unsigned char *buf, size_t cap) {
    DWORD got = 0;
    if (!ReadFile(sp->h, buf, (DWORD)cap, &got, NULL)) return -1;
    return (int)got;
}

int serial_write(SerialPort *sp, const unsigned char *buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        DWORD w = 0;
        if (!WriteFile(sp->h, buf + done, (DWORD)(n - done), &w, NULL) || w == 0) return -1;
        done += w;
    }
    return (int)n;
}

void serial_close(SerialPort *sp) {
    if (!sp) return;
    CloseHandle(sp->h);
    free(sp);
}
#else
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

struct SerialPort { int fd; };

static int baud_const(int baud) {
    switch (baud) {
        case 1200: return B1200; case 2400: return B2400; case 4800: return B4800; case 9600: return B9600;
        case 19200: return B19200; case 38400: return B38400; case 57600: return B57600;
        case 115200: return B115200; case 230400: return B230400;
        default: return -1;
    }
}

SerialPort *serial_open(const char *port, int baud, char *errbuf, size_t errlen) {
    struct termios t;
    int bc = baud_const(baud);
    SerialPort *sp;
    int fd;
    if (bc < 0) { snprintf(errbuf, errlen, "unsupported baud %d", baud); return NULL; }
    fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) { snprintf(errbuf, errlen, "open %s: %s", port, strerror(errno)); return NULL; }
    if (tcgetattr(fd, &t) != 0) { snprintf(errbuf, errlen, "tcgetattr %s: %s", port, strerror(errno)); close(fd); return NULL; }
    cfmakeraw(&t);
    cfsetispeed(&t, (speed_t)bc); cfsetospeed(&t, (speed_t)bc);
    t.c_cflag |= (CLOCAL | CREAD);
    t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &t) != 0) { snprintf(errbuf, errlen, "tcsetattr %s: %s", port, strerror(errno)); close(fd); return NULL; }
    sp = (SerialPort *)calloc(1, sizeof(*sp));
    if (!sp) { close(fd); return NULL; }
    sp->fd = fd;
    return sp;
}

int serial_read(SerialPort *sp, unsigned char *buf, size_t cap) {
    ssize_t n = read(sp->fd, buf, cap);
    if (n >= 0) return (int)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
}

int serial_write(SerialPort *sp, const unsigned char *buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(sp->fd, buf + done, n - done);
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { usleep(1000); continue; }
        if (w <= 0) return -1;
        done += (size_t)w;
    }
    return (int)n;
}

void serial_close(SerialPort *sp) {
    if (!sp) return;
    close(sp->fd);
    free(sp);
}
#endif
