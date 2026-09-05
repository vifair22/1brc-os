/* Minimal kprintf on top of console_putc. Supported conversions:
 *   %c %s %d %u %x %p %%
 * with 'l'/'ll'/'z' length modifiers (all widen to 64-bit), field width,
 * and zero padding. That is the entire vocabulary the kernel needs.
 */
#ifndef CONSOLE_FMT_H
#define CONSOLE_FMT_H

#include <stdarg.h>

void kprintf(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
void kvprintf(const char *fmt, va_list ap);

#endif
