/* tests/avr109_flash_cli.c -- tiny driver: avr109_flash_cli <hexfile> <port>. Parses the hex with the
 * client's real parser and flashes through the client's real avr109 code. Exit 0 ok, 1 parse error,
 * 2 flash error. Used by tests/test_avr109.py against a bootloader simulator. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../client/avr109.h"

static void prog(void *u, const char *m) { (void)u; fprintf(stderr, "[progress] %s\n", m); }

int main(int argc, char **argv) {
    static unsigned char image[AVR_FLASH_MAX];
    static char text[400000], err[256];
    FILE *f;
    size_t n;
    int len;
    SerialPort *sp;
    if (argc < 3) return 1;
    f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    n = fread(text, 1, sizeof(text) - 1, f); text[n] = 0; fclose(f);
    len = ihex_parse(text, image, sizeof(image), err, sizeof(err));
    if (len < 0) { fprintf(stderr, "parse: %s\n", err); return 1; }
    printf("image %d bytes\n", len);
    sp = serial_open(argv[2], 57600, err, sizeof(err));
    if (!sp) { fprintf(stderr, "open: %s\n", err); return 2; }
    if (avr109_flash(sp, image, len, prog, NULL, err, sizeof(err)) != 0) { fprintf(stderr, "flash: %s\n", err); return 2; }
    serial_close(sp);
    return 0;
}
