/* Framebuffer text console. The only output device the kernel has after
 * ExitBootServices; every byte is mirrored to debugcon for headless QEMU
 * runs.
 */
#ifndef CONSOLE_CONSOLE_H
#define CONSOLE_CONSOLE_H

#include "kernel/bootinfo.h"

void console_init(const fb_info *fb);
void console_putc(char c);
void console_write(const char *s);

#endif
