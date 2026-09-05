#include "console/fmt.h"

#include <stdint.h>

#include "console/console.h"

static void emit_padded(const char *buf, int len, int width, char pad)
{
    for (int i = len; i < width; i++)
        console_putc(pad);
    for (int i = 0; i < len; i++)
        console_putc(buf[i]);
}

static int format_u64(char *buf, uint64_t v, unsigned base, int negative)
{
    static const char digits[] = "0123456789abcdef";
    char tmp[24];
    int n = 0;

    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v);
    if (negative)
        tmp[n++] = '-';

    for (int i = 0; i < n; i++)
        buf[i] = tmp[n - 1 - i];
    return n;
}

void kvprintf(const char *fmt, va_list ap)
{
    while (*fmt) {
        if (*fmt != '%') {
            console_putc(*fmt++);
            continue;
        }
        fmt++;

        char pad = ' ';
        int width = 0;
        int longs = 0;

        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        while (*fmt == 'l' || *fmt == 'z') {
            longs = 1;
            fmt++;
        }

        char buf[24];
        int len;
        uint64_t v;

        switch (*fmt) {
        case 'c':
            buf[0] = (char)va_arg(ap, int);
            emit_padded(buf, 1, width, ' ');
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            len = 0;
            while (s[len])
                len++;
            emit_padded(s, len, width, ' ');
            break;
        }
        case 'd': {
            int64_t sv = longs ? va_arg(ap, int64_t)
                               : (int64_t)va_arg(ap, int32_t);
            v = sv < 0 ? (uint64_t)-sv : (uint64_t)sv;
            len = format_u64(buf, v, 10, sv < 0);
            emit_padded(buf, len, width, pad);
            break;
        }
        case 'u':
            v = longs ? va_arg(ap, uint64_t) : va_arg(ap, uint32_t);
            len = format_u64(buf, v, 10, 0);
            emit_padded(buf, len, width, pad);
            break;
        case 'x':
            v = longs ? va_arg(ap, uint64_t) : va_arg(ap, uint32_t);
            len = format_u64(buf, v, 16, 0);
            emit_padded(buf, len, width, pad);
            break;
        case 'p':
            v = (uint64_t)va_arg(ap, void *);
            console_putc('0');
            console_putc('x');
            len = format_u64(buf, v, 16, 0);
            emit_padded(buf, len, 16, '0');
            break;
        case '%':
            console_putc('%');
            break;
        default:
            /* Unknown conversion: print it raw so the bug is visible. */
            console_putc('%');
            if (*fmt)
                console_putc(*fmt);
            break;
        }
        if (*fmt)
            fmt++;
    }
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}
