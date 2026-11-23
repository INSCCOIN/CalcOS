/* Drawing built on nothing but plat_gfx_point, so the board layer and the
 * host layer cannot drift apart: a shape rendered in a test is the same
 * shape the panel gets. */
#include "calc.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* clipping                                                            */
/* ------------------------------------------------------------------ */

/* Every drawing call goes through plat_gfx_point, so the clip rectangle
 * lives here rather than in each primitive: set it once and text, sprites,
 * lines and circles all respect it. Without one, "draw this string at the
 * bottom of a panel" quietly runs off the edge, and a sprite near the right
 * margin wraps into the next row. */
static int clip_x0, clip_y0, clip_x1, clip_y1, clipping;

void plat_gfx_clip(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) { clipping = 1; clip_x0 = clip_y0 = 0;
                            clip_x1 = clip_y1 = -1; return; }
    clip_x0 = x; clip_y0 = y;
    clip_x1 = x + w - 1; clip_y1 = y + h - 1;
    clipping = 1;
}

void plat_gfx_noclip(void) { clipping = 0; }

void plat_gfx_point(int x, int y, unsigned rgb)
{
    if (clipping && (x < clip_x0 || y < clip_y0 || x > clip_x1 || y > clip_y1))
        return;
    plat_gfx_raw_point(x, y, rgb);
}

/* ------------------------------------------------------------------ */
/* sprites and images                                                  */
/* ------------------------------------------------------------------ */

/* A sprite is packed four bits to a pixel, two pixels to a byte, low
 * nibble first — the same packing the overlay uses, so an image can be
 * copied straight through. Index 0 is transparent, which is what makes a
 * sprite a sprite rather than a rectangle. */
void plat_gfx_sprite(int x, int y, int w, int h, const void *data,
                     const unsigned *palette, int npal)
{
    const unsigned char *d = (const unsigned char *)data;
    int row, col, stride;

    if (!d || !palette || w <= 0 || h <= 0) return;
    stride = (w + 1) / 2;
    for (row = 0; row < h; row++) {
        const unsigned char *p = d + (size_t)row * stride;
        for (col = 0; col < w; col++) {
            int idx = (col & 1) ? (p[col >> 1] >> 4) : (p[col >> 1] & 0x0F);
            if (idx == 0) continue;                 /* transparent */
            if (idx >= npal) continue;
            plat_gfx_point(x + col, y + row, palette[idx]);
        }
    }
}

/* The same, but index 0 is drawn too: for a background or a full picture. */
void plat_gfx_blit(int x, int y, int w, int h, const void *data,
                   const unsigned *palette, int npal)
{
    const unsigned char *d = (const unsigned char *)data;
    int row, col, stride;

    if (!d || !palette || w <= 0 || h <= 0) return;
    stride = (w + 1) / 2;
    for (row = 0; row < h; row++) {
        const unsigned char *p = d + (size_t)row * stride;
        for (col = 0; col < w; col++) {
            int idx = (col & 1) ? (p[col >> 1] >> 4) : (p[col >> 1] & 0x0F);
            if (idx < npal) plat_gfx_point(x + col, y + row, palette[idx]);
        }
    }
}

/* A stored 16-colour image: a small header, its own palette, then the
 * pixels. Returns 0 if the bytes are not an image. */
int plat_gfx_image(int x, int y, const void *blob, int len)
{
    const unsigned char *b = (const unsigned char *)blob;
    unsigned pal[16];
    int w, h, i, need;

    if (!b || len < (int)sizeof(GfxImageHdr)) return 0;
    if (memcmp(b, "CIM1", 4) != 0) return 0;
    w = b[4] | (b[5] << 8);
    h = b[6] | (b[7] << 8);
    if (w <= 0 || h <= 0) return 0;
    need = (int)sizeof(GfxImageHdr) + ((w + 1) / 2) * h;
    if (len < need) return 0;
    for (i = 0; i < 16; i++)
        pal[i] = ((unsigned)b[16 + i * 3] << 16)
               | ((unsigned)b[17 + i * 3] << 8)
               | (unsigned)b[18 + i * 3];
    plat_gfx_blit(x, y, w, h, b + sizeof(GfxImageHdr), pal, 16);
    return 1;
}

int plat_gfx_image_size(const void *blob, int len, int *w, int *h)
{
    const unsigned char *b = (const unsigned char *)blob;
    if (!b || len < (int)sizeof(GfxImageHdr) || memcmp(b, "CIM1", 4) != 0)
        return 0;
    if (w) *w = b[4] | (b[5] << 8);
    if (h) *h = b[6] | (b[7] << 8);
    return 1;
}

void plat_gfx_line(int x0, int y0, int x1, int y1, unsigned rgb)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        plat_gfx_point(x0, y0, rgb);
        if (x0 == x1 && y0 == y1) break;
        {
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
}

void plat_gfx_rect(int x, int y, int w, int h, unsigned rgb)
{
    if (w <= 0 || h <= 0) return;
    plat_gfx_line(x, y, x + w - 1, y, rgb);
    plat_gfx_line(x, y + h - 1, x + w - 1, y + h - 1, rgb);
    plat_gfx_line(x, y, x, y + h - 1, rgb);
    plat_gfx_line(x + w - 1, y, x + w - 1, y + h - 1, rgb);
}

void plat_gfx_fill_rect(int x, int y, int w, int h, unsigned rgb)
{
    int yy, xx;
    if (w <= 0 || h <= 0) return;
    for (yy = y; yy < y + h; yy++)
        for (xx = x; xx < x + w; xx++)
            plat_gfx_point(xx, yy, rgb);
}

/* Midpoint circle: integer only, no square roots. */
void plat_gfx_circle(int cx, int cy, int r, unsigned rgb)
{
    int x = r, y = 0, err = 1 - r;
    if (r < 0) return;
    if (r == 0) { plat_gfx_point(cx, cy, rgb); return; }
    while (x >= y) {
        plat_gfx_point(cx + x, cy + y, rgb);
        plat_gfx_point(cx + y, cy + x, rgb);
        plat_gfx_point(cx - y, cy + x, rgb);
        plat_gfx_point(cx - x, cy + y, rgb);
        plat_gfx_point(cx - x, cy - y, rgb);
        plat_gfx_point(cx - y, cy - x, rgb);
        plat_gfx_point(cx + y, cy - x, rgb);
        plat_gfx_point(cx + x, cy - y, rgb);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void plat_gfx_fill_circle(int cx, int cy, int r, unsigned rgb)
{
    int x = r, y = 0, err = 1 - r;
    if (r < 0) return;
    while (x >= y) {
        plat_gfx_line(cx - x, cy + y, cx + x, cy + y, rgb);
        plat_gfx_line(cx - x, cy - y, cx + x, cy - y, rgb);
        plat_gfx_line(cx - y, cy + x, cx + y, cy + x, rgb);
        plat_gfx_line(cx - y, cy - x, cx + y, cy - x, rgb);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}
