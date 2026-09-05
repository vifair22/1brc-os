#include "console/console.h"

#include <stdint.h>

#include "console/debugcon.h"
#include "console/font8x16.h"
#include "lib/string.h"

/* Colors are stored channel-order-agnostic as (r,g,b) and swizzled once
 * at init according to the GOP pixel format, so the hot putpixel path is
 * a single 32-bit store.
 */

#define MARGIN_PX 8

static struct {
    volatile uint32_t *base;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t cols;
    uint32_t rows;
    uint32_t cur_col;
    uint32_t cur_row;
    uint32_t fg;
    uint32_t bg;
    int ready;
} con;

static uint32_t pack_color(const fb_info *fb, uint8_t r, uint8_t g, uint8_t b)
{
    if (fb->format == 0) /* red in byte 0 */
        return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16);
    return (uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16);
}

void console_init(const fb_info *fb)
{
    con.base = (volatile uint32_t *)fb->base;
    con.width = fb->width;
    con.height = fb->height;
    con.stride = fb->stride;
    con.cols = (fb->width - 2 * MARGIN_PX) / FONT_WIDTH;
    con.rows = (fb->height - 2 * MARGIN_PX) / FONT_HEIGHT;
    con.cur_col = 0;
    con.cur_row = 0;
    con.fg = pack_color(fb, 0xd0, 0xd0, 0xd0);
    con.bg = pack_color(fb, 0x10, 0x10, 0x18);

    for (uint32_t y = 0; y < con.height; y++)
        for (uint32_t x = 0; x < con.width; x++)
            con.base[y * con.stride + x] = con.bg;

    con.ready = 1;
}

static void draw_glyph(uint32_t col, uint32_t row, char c)
{
    const uint8_t *glyph = font8x16[(uint8_t)c];
    uint32_t px = MARGIN_PX + col * FONT_WIDTH;
    uint32_t py = MARGIN_PX + row * FONT_HEIGHT;

    for (uint32_t y = 0; y < FONT_HEIGHT; y++) {
        volatile uint32_t *line = con.base + (py + y) * con.stride + px;
        uint8_t bits = glyph[y];
        for (uint32_t x = 0; x < FONT_WIDTH; x++)
            line[x] = (bits & (0x80u >> x)) ? con.fg : con.bg;
    }
}

static void scroll(void)
{
    /* One text row = FONT_HEIGHT scanlines. memmove is fine here: output
     * volume is a few hundred lines total, not a hot path. */
    uint32_t row_px = FONT_HEIGHT * con.stride;
    uint32_t body_rows = (con.rows - 1) * FONT_HEIGHT;

    /* The volatile qualifier is shed via uintptr_t: nothing else
     * observes the framebuffer during the copy, and memmove on it is an
     * order of magnitude faster than a volatile-qualified loop. */
    memmove((void *)(uintptr_t)(con.base + MARGIN_PX * con.stride),
            (const void *)(uintptr_t)(con.base + MARGIN_PX * con.stride +
                                      row_px),
            (size_t)body_rows * con.stride * 4);

    volatile uint32_t *last =
        con.base + (MARGIN_PX + body_rows) * con.stride;
    for (uint32_t i = 0; i < row_px; i++)
        last[i] = con.bg;
}

void console_putc(char c)
{
    debugcon_putc(c);
    if (!con.ready)
        return;

    if (c == '\n') {
        con.cur_col = 0;
        con.cur_row++;
    } else if (c == '\r') {
        con.cur_col = 0;
    } else {
        if (con.cur_col >= con.cols) {
            con.cur_col = 0;
            con.cur_row++;
        }
        if (con.cur_row >= con.rows) {
            scroll();
            con.cur_row = con.rows - 1;
        }
        draw_glyph(con.cur_col++, con.cur_row, c);
        return;
    }

    if (con.cur_row >= con.rows) {
        scroll();
        con.cur_row = con.rows - 1;
    }
}

void console_write(const char *s)
{
    while (*s)
        console_putc(*s++);
}
