/* client/serial_port.h -- minimal cross-platform serial port for the cabinet client (card #492/#474):
 * open a COM port / tty at a baud rate, non-blocking read, write. Win32 backend uses CreateFile +
 * SetCommState (compiled by mingw, NOT yet run on real Windows); POSIX backend uses termios (tested in
 * `make test-e2e` against a pty standing in for the Feather). */
#ifndef EDGE_SERIAL_PORT_H
#define EDGE_SERIAL_PORT_H

#include <stddef.h>

typedef struct SerialPort SerialPort;

/* port is "COM5" / "\\\\.\\COM12" / "/dev/ttyACM0". Returns NULL on failure (errbuf gets the reason). */
SerialPort *serial_open(const char *port, int baud, char *errbuf, size_t errlen);
/* Reads whatever is available right now (never blocks). Returns bytes read, 0 if none, -1 on error. */
int serial_read(SerialPort *sp, unsigned char *buf, size_t cap);
/* Writes all n bytes. Returns n or -1. */
int serial_write(SerialPort *sp, const unsigned char *buf, size_t n);
void serial_close(SerialPort *sp);

#endif
