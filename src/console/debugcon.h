/* QEMU isa-debugcon mirror: every console byte is also written to I/O
 * port 0xE9, which QEMU can log to a file or stdio. On real hardware
 * the port write is a harmless no-op, so this stays unconditional.
 */
#ifndef CONSOLE_DEBUGCON_H
#define CONSOLE_DEBUGCON_H

void debugcon_putc(char c);
void debugcon_write(const char *s);

#endif
