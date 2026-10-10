/* client/avr109.c -- see avr109.h. */
#define _DEFAULT_SOURCE 1
#include "avr109.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <unistd.h>
static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
#endif

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hexbyte(const char *p) {
    int a = hexv(p[0]), b = a < 0 ? -1 : hexv(p[1]);
    return b < 0 ? -1 : a * 16 + b;
}

int ihex_parse(const char *text, unsigned char *image, size_t cap, char *errbuf, size_t errlen) {
    size_t max_end = 0;
    unsigned long ext = 0; /* upper address bits from type 02/04 records */
    int lineno = 0;
    const char *p = text;
    memset(image, 0xFF, cap);
    while (*p) {
        const char *eol = p;
        int len, hi, lo, type, sum, i;
        unsigned long addr;
        while (*eol && *eol != '\n') eol++;
        lineno++;
        while (*p == ' ' || *p == '\r') p++;
        if (*p != ':' ) { if (p == eol) { p = *eol ? eol + 1 : eol; continue; } snprintf(errbuf, errlen, "line %d: not an Intel HEX record", lineno); return -1; }
        len = hexbyte(p + 1); hi = hexbyte(p + 3); lo = hexbyte(p + 5); type = hexbyte(p + 7);
        if (len < 0 || hi < 0 || lo < 0 || type < 0) { snprintf(errbuf, errlen, "line %d: bad hex digits", lineno); return -1; }
        sum = len + hi + lo + type;
        for (i = 0; i < len; i++) { int b = hexbyte(p + 9 + 2 * i); if (b < 0) { snprintf(errbuf, errlen, "line %d: bad data", lineno); return -1; } sum += b; }
        { int ck = hexbyte(p + 9 + 2 * len); if (ck < 0 || ((sum + ck) & 0xFF) != 0) { snprintf(errbuf, errlen, "line %d: checksum mismatch", lineno); return -1; } }
        addr = ext + (unsigned long)(hi * 256 + lo);
        if (type == 0) {
            if (addr + (unsigned long)len > cap) { snprintf(errbuf, errlen, "line %d: address 0x%lx past the %zu-byte app flash", lineno, addr + (unsigned long)len, cap); return -1; }
            for (i = 0; i < len; i++) image[addr + (unsigned long)i] = (unsigned char)hexbyte(p + 9 + 2 * i);
            if (addr + (unsigned long)len > max_end) max_end = addr + (unsigned long)len;
        } else if (type == 1) {
            break;
        } else if (type == 2) {
            ext = (unsigned long)(hexbyte(p + 9) * 256 + hexbyte(p + 11)) * 16UL;
        } else if (type == 4) {
            ext = (unsigned long)(hexbyte(p + 9) * 256 + hexbyte(p + 11)) << 16;
        }
        p = *eol ? eol + 1 : eol;
    }
    if (max_end == 0) { snprintf(errbuf, errlen, "hex file contained no data"); return -1; }
    return (int)max_end;
}

/* read exactly n bytes within timeout_ms */
static int read_exact(SerialPort *sp, unsigned char *buf, int n, int timeout_ms) {
    int got = 0, waited = 0;
    while (got < n) {
        int r = serial_read(sp, buf + got, (size_t)(n - got));
        if (r < 0) return -1;
        if (r > 0) { got += r; continue; }
        if (waited >= timeout_ms) return got;
        sleep_ms(2); waited += 2;
    }
    return got;
}

static int expect_cr(SerialPort *sp, const char *what, char *errbuf, size_t errlen) {
    unsigned char c = 0;
    int r = read_exact(sp, &c, 1, 2000);
    if (r == 1 && c == '\r') return 0;
    snprintf(errbuf, errlen, "bootloader did not acknowledge %s (got %s0x%02x)", what, r == 1 ? "" : "nothing/", r == 1 ? c : 0);
    return -1;
}

static int set_address(SerialPort *sp, int byte_addr, char *errbuf, size_t errlen) {
    unsigned char cmd[3];
    int w = byte_addr / 2;
    cmd[0] = 'A'; cmd[1] = (unsigned char)(w >> 8); cmd[2] = (unsigned char)(w & 0xFF);
    if (serial_write(sp, cmd, 3) < 0) { snprintf(errbuf, errlen, "write failed"); return -1; }
    return expect_cr(sp, "set-address", errbuf, errlen);
}

int avr109_flash(SerialPort *sp, const unsigned char *image, int len, Avr109Progress progress, void *user,
                 char *errbuf, size_t errlen) {
    unsigned char id[8], tmp[4], page[AVR_PAGE + 4], back[AVR_PAGE];
    int off, pages;
    char msg[96];
#define PROG(m) do { if (progress) progress(user, (m)); } while (0)
    if (len <= 0 || len > AVR_FLASH_MAX) { snprintf(errbuf, errlen, "image length %d out of range", len); return -1; }
    /* drop anything the bootloader said on connect */
    while (serial_read(sp, tmp, sizeof(tmp)) > 0) {}

    if (serial_write(sp, (const unsigned char *)"S", 1) < 0) { snprintf(errbuf, errlen, "write failed"); return -1; }
    if (read_exact(sp, id, 7, 2000) != 7 || memcmp(id, "CATERIN", 7) != 0) {
        snprintf(errbuf, errlen, "no Caterina bootloader answered 'S' (is the Feather in bootloader mode? double-tap RESET)");
        return -1;
    }
    PROG("bootloader: CATERIN");
    if (serial_write(sp, (const unsigned char *)"p", 1) < 0 || read_exact(sp, tmp, 1, 1000) != 1) { snprintf(errbuf, errlen, "no reply to programmer-type query"); return -1; }
    if (serial_write(sp, (const unsigned char *)"b", 1) < 0 || read_exact(sp, tmp, 3, 1000) != 3 || tmp[0] != 'Y') {
        snprintf(errbuf, errlen, "bootloader does not support block mode"); return -1;
    }
    { int bs = tmp[1] * 256 + tmp[2]; if (bs < AVR_PAGE) { snprintf(errbuf, errlen, "bootloader block size %d < page size %d", bs, AVR_PAGE); return -1; } }
    if (serial_write(sp, (const unsigned char *)"T\x44", 2) < 0 || expect_cr(sp, "set-device", errbuf, errlen) < 0) return -1;
    if (serial_write(sp, (const unsigned char *)"P", 1) < 0 || expect_cr(sp, "enter-programming", errbuf, errlen) < 0) return -1;

    pages = (len + AVR_PAGE - 1) / AVR_PAGE;
    for (off = 0; off < pages * AVR_PAGE; off += AVR_PAGE) {
        snprintf(msg, sizeof(msg), "writing page %d/%d", off / AVR_PAGE + 1, pages);
        PROG(msg);
        if (set_address(sp, off, errbuf, errlen) < 0) return -1;
        page[0] = 'B'; page[1] = 0; page[2] = (unsigned char)AVR_PAGE; page[3] = 'F';
        memset(page + 4, 0xFF, AVR_PAGE);
        memcpy(page + 4, image + off, (size_t)(len - off < AVR_PAGE ? len - off : AVR_PAGE));
        if (serial_write(sp, page, 4 + AVR_PAGE) < 0) { snprintf(errbuf, errlen, "write failed"); return -1; }
        if (expect_cr(sp, "page write", errbuf, errlen) < 0) return -1;
    }
    PROG("verifying");
    for (off = 0; off < pages * AVR_PAGE; off += AVR_PAGE) {
        unsigned char rd[4];
        int want = len - off < AVR_PAGE ? len - off : AVR_PAGE;
        if (set_address(sp, off, errbuf, errlen) < 0) return -1;
        rd[0] = 'g'; rd[1] = 0; rd[2] = (unsigned char)AVR_PAGE; rd[3] = 'F';
        if (serial_write(sp, rd, 4) < 0) { snprintf(errbuf, errlen, "write failed"); return -1; }
        if (read_exact(sp, back, AVR_PAGE, 2000) != AVR_PAGE) { snprintf(errbuf, errlen, "short read-back at 0x%04x", off); return -1; }
        if (memcmp(back, image + off, (size_t)want) != 0) { snprintf(errbuf, errlen, "VERIFY FAILED in page at 0x%04x", off); return -1; }
    }
    if (serial_write(sp, (const unsigned char *)"L", 1) < 0 || expect_cr(sp, "leave-programming", errbuf, errlen) < 0) return -1;
    if (serial_write(sp, (const unsigned char *)"E", 1) < 0 || expect_cr(sp, "exit-bootloader", errbuf, errlen) < 0) return -1;
    PROG("done: bootloader exited, sketch starting");
    return 0;
#undef PROG
}
