/* client/usb_probe.h -- EDGE.GAME USB/COM probe (card #493; founder real-time 2026-10-02: "build
 * tooling in to probe the windows os for the usb controller to find it ... very batteries included
 * training platform for the orphans"). Finds the Feather (and Nano/Pi gadget) by USB VID/PID,
 * reports which COM port it is on and whether it is running a sketch or sitting in its Caterina
 * bootloader. Pure classification + JSON are portable and unit-tested (tests/test_usb_probe.c);
 * enumeration has two backends: Windows (registry, no SetupAPI -- see usb_probe.c) and POSIX
 * (/sys/class/tty, used by the tests against a fake tree). */
#ifndef EDGE_USB_PROBE_H
#define EDGE_USB_PROBE_H

#include <stddef.h>

typedef enum {
    USB_ROLE_UNKNOWN = 0,
    USB_ROLE_FEATHER_APP,        /* Adafruit Feather 32u4 running a sketch (CDC serial) */
    USB_ROLE_FEATHER_BOOTLOADER, /* Feather 32u4 in Caterina bootloader (double-tap reset) */
    USB_ROLE_NANO_CH340,
    USB_ROLE_NANO_FTDI,
    USB_ROLE_ARDUINO_OTHER,
    USB_ROLE_PI_GADGET
} UsbRole;

typedef struct {
    unsigned vid, pid;
    char port[32];  /* "COM5" / "/dev/ttyACM0"; empty if no port */
    char desc[96];  /* friendly name if the OS gave one */
    UsbRole role;
} UsbDev;

#define USB_PROBE_MAX 32

UsbRole usb_probe_classify(unsigned vid, unsigned pid);
const char *usb_probe_role_name(UsbRole r);
/* Windows: registry. POSIX: sys_root is the sysfs root ("/sys", or a fake tree in tests). */
int usb_probe_enumerate(UsbDev *out, int max, const char *sys_root);
/* {"os":..,"count":N,"devices":[{..}],"feather":{..}|null,"hint":".."} -- returns length or -1. */
int usb_probe_json(const UsbDev *devs, int n, char *out, size_t outlen);

#endif
