#define _POSIX_C_SOURCE 200809L /* opendir/readdir under -std=c99 (must precede all includes) */
/* client/usb_probe.c -- see usb_probe.h. Windows backend is registry-based on purpose: it needs
 * only advapi32 (no SetupAPI/cfgmgr32 headers or import libs), so the same mingw one-liner that
 * builds the rest of the client builds this. NOT run on real Windows in this sandbox (no Windows
 * here) -- it is compiled by mingw in CI and must be proven on the founder's PC; `edge_client
 * probe` prints raw findings so a first run is self-diagnosing. */
#include "usb_probe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

UsbRole usb_probe_classify(unsigned vid, unsigned pid) {
    if (vid == 0x239A) {
        if (pid == 0x800C) return USB_ROLE_FEATHER_APP;
        if (pid == 0x000C) return USB_ROLE_FEATHER_BOOTLOADER;
        return USB_ROLE_ARDUINO_OTHER; /* some other Adafruit board */
    }
    if (vid == 0x1A86 && pid == 0x7523) return USB_ROLE_NANO_CH340;
    if (vid == 0x0403 && pid == 0x6001) return USB_ROLE_NANO_FTDI;
    if (vid == 0x2341 || vid == 0x2A03 || vid == 0x1B4F) return USB_ROLE_ARDUINO_OTHER;
    if ((vid == 0x0525 && pid == 0xA4A7) || (vid == 0x1D6B && pid == 0x0104)) return USB_ROLE_PI_GADGET;
    return USB_ROLE_UNKNOWN;
}

const char *usb_probe_role_name(UsbRole r) {
    switch (r) {
        case USB_ROLE_FEATHER_APP: return "feather_app";
        case USB_ROLE_FEATHER_BOOTLOADER: return "feather_bootloader";
        case USB_ROLE_NANO_CH340: return "nano_ch340";
        case USB_ROLE_NANO_FTDI: return "nano_ftdi";
        case USB_ROLE_ARDUINO_OTHER: return "arduino_other";
        case USB_ROLE_PI_GADGET: return "pi_gadget";
        default: return "unknown";
    }
}

static void json_escape(const char *in, char *out, size_t outlen) {
    size_t o = 0;
    for (; *in && o + 7 < outlen; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20 || c > 0x7e) out[o++] = '?';
        else out[o++] = (char)c;
    }
    out[o] = '\0';
}

int usb_probe_json(const UsbDev *devs, int n, char *out, size_t outlen) {
    size_t o = 0;
    int feather = -1;
    int i;
#define APPEND(...) do { \
        int w_ = snprintf(out + o, outlen - o, __VA_ARGS__); \
        if (w_ < 0 || (size_t)w_ >= outlen - o) { return -1; } \
        o += (size_t)w_; \
    } while (0)
#ifdef _WIN32
    APPEND("{\"os\":\"windows\",\"count\":%d,\"devices\":[", n);
#else
    APPEND("{\"os\":\"posix\",\"count\":%d,\"devices\":[", n);
#endif
    for (i = 0; i < n; i++) {
        char d[200], p[80];
        json_escape(devs[i].desc, d, sizeof(d));
        json_escape(devs[i].port, p, sizeof(p));
        APPEND("%s{\"vid\":\"%04X\",\"pid\":\"%04X\",\"port\":\"%s\",\"desc\":\"%s\",\"role\":\"%s\"}",
               i ? "," : "", devs[i].vid, devs[i].pid, p, d, usb_probe_role_name(devs[i].role));
        if (feather < 0 && (devs[i].role == USB_ROLE_FEATHER_APP || devs[i].role == USB_ROLE_FEATHER_BOOTLOADER))
            feather = i;
    }
    APPEND("],");
    if (feather >= 0) {
        char p[80];
        json_escape(devs[feather].port, p, sizeof(p));
        APPEND("\"feather\":{\"port\":\"%s\",\"state\":\"%s\"},", p,
               devs[feather].role == USB_ROLE_FEATHER_APP ? "running_sketch" : "bootloader");
        APPEND("\"hint\":\"%s\"}",
               devs[feather].role == USB_ROLE_FEATHER_APP
                   ? "Feather found running a sketch; ready for serial."
                   : "Feather is in its bootloader (ready to flash); it leaves it by itself after ~8s or on reset.");
    } else {
        APPEND("\"feather\":null,\"hint\":\"%s\"}",
               n == 0 ? "No USB serial devices at all: check the cable is a DATA cable (not charge-only), try another port, and on Windows confirm Device Manager shows it under Ports (COM & LPT) or Other devices."
                      : "USB serial devices found but none is a Feather 32u4 (VID 239A PID 800C / 000C). Replug it, or double-tap RESET to enter the bootloader.");
    }
    return (int)o;
#undef APPEND
}

#ifdef _WIN32
#include <windows.h>

/* Enum\USB\VID_xxxx&PID_xxxx[&MI_xx]\<instance>\Device Parameters\PortName names the COM port of
 * every device Windows has EVER seen, so liveness comes from HKLM\HARDWARE\DEVICEMAP\SERIALCOMM,
 * which only lists ports that exist right now. */
static int live_ports(char names[][32], int max) {
    HKEY k;
    DWORD i, cnt = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &k) != ERROR_SUCCESS) return 0;
    for (i = 0; cnt < (DWORD)max; i++) {
        char vn[256]; BYTE data[64]; DWORD vnl = sizeof(vn), dl = sizeof(data) - 1, type;
        if (RegEnumValueA(k, i, vn, &vnl, NULL, &type, data, &dl) != ERROR_SUCCESS) break;
        if (type != REG_SZ) continue;
        data[dl] = 0;
        snprintf(names[cnt++], 32, "%s", (char *)data);
    }
    RegCloseKey(k);
    return (int)cnt;
}

static int reg_str(HKEY root, const char *sub, const char *val, char *out, size_t outlen) {
    HKEY k; DWORD type, len = (DWORD)outlen - 1;
    if (RegOpenKeyExA(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return 0;
    LONG r = RegQueryValueExA(k, val, NULL, &type, (BYTE *)out, &len);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || type != REG_SZ) return 0;
    out[len] = 0;
    return 1;
}

int usb_probe_enumerate(UsbDev *out, int max, const char *sys_root) {
    char live[USB_PROBE_MAX][32];
    int nlive = live_ports(live, USB_PROBE_MAX), n = 0, li;
    HKEY usb;
    DWORD a;
    (void)sys_root;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Enum\\USB", 0, KEY_READ, &usb) != ERROR_SUCCESS)
        return 0;
    for (a = 0; n < max; a++) {
        char devkey[128]; DWORD dl = sizeof(devkey);
        unsigned vid = 0, pid = 0;
        HKEY dk; DWORD b;
        if (RegEnumKeyExA(usb, a, devkey, &dl, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (sscanf(devkey, "VID_%x&PID_%x", &vid, &pid) != 2) continue;
        if (RegOpenKeyExA(usb, devkey, 0, KEY_READ, &dk) != ERROR_SUCCESS) continue;
        for (b = 0; n < max; b++) {
            char inst[128], path[512], port[32] = "", desc[96] = "", raw[160] = "";
            DWORD il = sizeof(inst);
            if (RegEnumKeyExA(dk, b, inst, &il, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
            snprintf(path, sizeof(path), "SYSTEM\\CurrentControlSet\\Enum\\USB\\%s\\%s\\Device Parameters", devkey, inst);
            if (!reg_str(HKEY_LOCAL_MACHINE, path, "PortName", port, sizeof(port))) continue;
            for (li = 0; li < nlive; li++) if (strcmp(live[li], port) == 0) break;
            if (li == nlive) continue; /* historical device, not plugged in now */
            snprintf(path, sizeof(path), "SYSTEM\\CurrentControlSet\\Enum\\USB\\%s\\%s", devkey, inst);
            if (reg_str(HKEY_LOCAL_MACHINE, path, "FriendlyName", raw, sizeof(raw)) ||
                reg_str(HKEY_LOCAL_MACHINE, path, "DeviceDesc", raw, sizeof(raw))) {
                const char *semi = strrchr(raw, ';'); /* DeviceDesc looks like "@oem1.inf,...;Name" */
                snprintf(desc, sizeof(desc), "%s", semi ? semi + 1 : raw);
            }
            out[n].vid = vid; out[n].pid = pid;
            snprintf(out[n].port, sizeof(out[n].port), "%s", port);
            snprintf(out[n].desc, sizeof(out[n].desc), "%s", desc);
            out[n].role = usb_probe_classify(vid, pid);
            n++;
        }
        RegCloseKey(dk);
    }
    RegCloseKey(usb);
    return n;
}
#else
#include <dirent.h>

static int read_hex(const char *path, unsigned *v) {
    char buf[32];
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return 0; }
    fclose(f);
    *v = (unsigned)strtoul(buf, NULL, 16);
    return 1;
}

/* /sys/class/tty/ttyACM0/device is a symlink into the USB interface dir; idVendor/idProduct sit
 * one level up (the USB device dir). Tests build a fake tree where "device/../idVendor" resolves
 * with plain directories, so the same path arithmetic runs on both. */
int usb_probe_enumerate(UsbDev *out, int max, const char *sys_root) {
    char dirp[512];
    DIR *d;
    struct dirent *e;
    int n = 0;
    snprintf(dirp, sizeof(dirp), "%s/class/tty", sys_root ? sys_root : "/sys");
    d = opendir(dirp);
    if (!d) return 0;
    while ((e = readdir(d)) != NULL && n < max) {
        char p[1100], prod[96];
        unsigned vid, pid;
        if (strncmp(e->d_name, "ttyACM", 6) != 0 && strncmp(e->d_name, "ttyUSB", 6) != 0) continue;
        if (strlen(e->d_name) > 20) continue; /* real names are ttyACM12-sized */
        snprintf(p, sizeof(p), "%s/%s/device/../idVendor", dirp, e->d_name);
        if (!read_hex(p, &vid)) continue;
        snprintf(p, sizeof(p), "%s/%s/device/../idProduct", dirp, e->d_name);
        if (!read_hex(p, &pid)) continue;
        out[n].vid = vid; out[n].pid = pid;
        snprintf(out[n].port, sizeof(out[n].port), "/dev/%.20s", e->d_name);
        out[n].desc[0] = '\0';
        snprintf(p, sizeof(p), "%s/%s/device/../product", dirp, e->d_name);
        {
            FILE *f = fopen(p, "r");
            if (f) {
                if (fgets(prod, sizeof(prod), f)) {
                    prod[strcspn(prod, "\r\n")] = 0;
                    snprintf(out[n].desc, sizeof(out[n].desc), "%s", prod);
                }
                fclose(f);
            }
        }
        out[n].role = usb_probe_classify(vid, pid);
        n++;
    }
    closedir(d);
    return n;
}
#endif
