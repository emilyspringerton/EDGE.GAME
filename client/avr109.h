/* client/avr109.h -- AVR109 (butterfly/Caterina) flasher + Intel HEX parser for the Feather 32u4
 * (cards #477/#474). Lets the cabinet client flash a .hex that the SERVER compiled, so a Windows PC
 * needs neither avr-gcc nor avrdude. Protocol per AVR109/AVR911 as spoken by Adafruit's Caterina
 * bootloader (what `avrdude -c avr109` talks to). Verified here only against a protocol simulator
 * (tests/avr109_sim.py); never run against a real Feather yet. */
#ifndef EDGE_AVR109_H
#define EDGE_AVR109_H

#include <stddef.h>
#include "serial_port.h"

#define AVR_FLASH_MAX 28672   /* ATmega32u4 app section under Caterina (bootloader owns the top 4K) */
#define AVR_PAGE 128

/* Parse Intel HEX text into image[] (0xFF-filled up to the highest address). Returns image length
 * (>0) or -1 with errbuf set (bad record, checksum, address past AVR_FLASH_MAX). */
int ihex_parse(const char *text, unsigned char *image, size_t cap, char *errbuf, size_t errlen);

/* Flash image[0..len) to a port already open in the bootloader, with read-back verify.
 * progress(user, msg) is called for each stage. Returns 0 ok, -1 on any failure (errbuf says why). */
typedef void (*Avr109Progress)(void *user, const char *msg);
int avr109_flash(SerialPort *sp, const unsigned char *image, int len, Avr109Progress progress, void *user,
                 char *errbuf, size_t errlen);

#endif
