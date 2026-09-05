#include "console/debugcon.h"

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t v)
{
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

void debugcon_putc(char c)
{
    outb(0xe9, (uint8_t)c);
}

void debugcon_write(const char *s)
{
    while (*s)
        debugcon_putc(*s++);
}
