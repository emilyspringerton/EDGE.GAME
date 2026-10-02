/* tests/test_usb_probe.c -- card #493. Classification + JSON + POSIX enumeration over a fake sysfs
 * tree built in a temp dir. The Windows registry backend is NOT covered here (no Windows in this
 * sandbox); it is compile-checked by the mingw `client-windows` target only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "usb_probe.h"

static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } else printf("PASS: %s\n", m); } while (0)

static void put(const char *root, const char *rel, const char *content) {
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", root, rel);
    FILE *f = fopen(p, "w");
    if (f) { fputs(content, f); fclose(f); }
}
static void dir(const char *root, const char *rel) {
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", root, rel);
    mkdir(p, 0755);
}

int main(void) {
    CHECK(usb_probe_classify(0x239A, 0x800C) == USB_ROLE_FEATHER_APP, "239A:800C is Feather app");
    CHECK(usb_probe_classify(0x239A, 0x000C) == USB_ROLE_FEATHER_BOOTLOADER, "239A:000C is Feather bootloader");
    CHECK(usb_probe_classify(0x1A86, 0x7523) == USB_ROLE_NANO_CH340, "1A86:7523 is CH340 Nano");
    CHECK(usb_probe_classify(0x0403, 0x6001) == USB_ROLE_NANO_FTDI, "0403:6001 is FTDI Nano");
    CHECK(usb_probe_classify(0x1D6B, 0x0104) == USB_ROLE_PI_GADGET, "1D6B:0104 is Pi gadget");
    CHECK(usb_probe_classify(0x1234, 0x5678) == USB_ROLE_UNKNOWN, "random VID/PID unknown");

    char root[] = "/tmp/usbprobeXXXXXX";
    if (!mkdtemp(root)) { printf("FAIL: mkdtemp\n"); return 1; }
    dir(root, "class"); dir(root, "class/tty");
    dir(root, "class/tty/ttyACM0"); dir(root, "class/tty/ttyACM0/device");
    put(root, "class/tty/ttyACM0/idVendor", "239a\n");
    put(root, "class/tty/ttyACM0/idProduct", "800c\n");
    put(root, "class/tty/ttyACM0/product", "Feather 32u4\n");
    dir(root, "class/tty/ttyUSB0"); dir(root, "class/tty/ttyUSB0/device");
    put(root, "class/tty/ttyUSB0/idVendor", "1a86\n");
    put(root, "class/tty/ttyUSB0/idProduct", "7523\n");
    dir(root, "class/tty/tty0"); /* non-USB tty must be ignored */

    UsbDev devs[USB_PROBE_MAX];
    int n = usb_probe_enumerate(devs, USB_PROBE_MAX, root);
    CHECK(n == 2, "enumerates exactly the two USB ttys");
    int fi = -1, i;
    for (i = 0; i < n; i++) if (devs[i].role == USB_ROLE_FEATHER_APP) fi = i;
    CHECK(fi >= 0 && strcmp(devs[fi].port, "/dev/ttyACM0") == 0, "Feather found on /dev/ttyACM0");
    CHECK(fi >= 0 && strcmp(devs[fi].desc, "Feather 32u4") == 0, "product string read");

    char js[4096];
    CHECK(usb_probe_json(devs, n, js, sizeof(js)) > 0, "json renders");
    CHECK(strstr(js, "\"feather\":{\"port\":\"/dev/ttyACM0\",\"state\":\"running_sketch\"}") != NULL, "json names the Feather + state");
    CHECK(strstr(js, "\"role\":\"nano_ch340\"") != NULL, "json lists the Nano");
    CHECK(usb_probe_json(devs, n, js, 20) == -1, "tiny buffer -> -1, no overflow");

    UsbDev boot = { 0x239A, 0x000C, "COM7", "Feather 32u4 bootloader", USB_ROLE_FEATHER_BOOTLOADER };
    usb_probe_json(&boot, 1, js, sizeof(js));
    CHECK(strstr(js, "\"state\":\"bootloader\"") != NULL, "bootloader state reported");
    usb_probe_json(devs, 0, js, sizeof(js));
    CHECK(strstr(js, "\"feather\":null") != NULL && strstr(js, "DATA cable") != NULL, "empty probe gives the data-cable hint");
    UsbDev q = { 0x239A, 0x800C, "COM\"1", "ev\\il", USB_ROLE_FEATHER_APP };
    usb_probe_json(&q, 1, js, sizeof(js));
    CHECK(strstr(js, "COM\\\"1") != NULL && strstr(js, "ev\\\\il") != NULL, "quotes/backslashes escaped");

    { char cmd[600]; snprintf(cmd, sizeof(cmd), "rm -rf %s", root); if (system(cmd)) {} }
    printf("%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
