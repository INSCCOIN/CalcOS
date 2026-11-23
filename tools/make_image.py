#!/usr/bin/env python3
"""Turn a picture into a 16-colour image the calculator can draw.

    python3 tools/make_image.py in.png out.cim [WIDTH HEIGHT]

The output is four bits a pixel with its own palette, which is the same
packing the graphics overlay uses, so drawing one is a copy. Put it in the
flash store with 'store put NAME file.cim' and draw it with
plot_image(x, y, "NAME") — it is read in place, so the picture costs no RAM
however large it is.

Index 0 is treated as transparent by plot_sprite, so the quantiser keeps it
for the most common colour only if the image has no alpha; with alpha, index
0 is the transparent colour.
"""
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("this needs Pillow: pip install pillow")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    im = Image.open(src)
    if len(sys.argv) >= 5:
        im = im.resize((int(sys.argv[3]), int(sys.argv[4])), Image.LANCZOS)

    has_alpha = im.mode in ("RGBA", "LA") or "transparency" in im.info
    rgba = im.convert("RGBA")
    w, h = rgba.size
    if w > 320 or h > 320:
        print("warning: %dx%d is larger than the panel" % (w, h))

    # quantise to 15 colours, leaving index 0 for transparency when needed
    want = 15 if has_alpha else 16
    flat = Image.new("RGB", (w, h), (0, 0, 0))
    flat.paste(rgba, mask=rgba.split()[3] if has_alpha else None)
    pal_img = flat.convert("P", palette=Image.ADAPTIVE, colors=want)
    pal = (pal_img.getpalette() or [])[: want * 3]
    pal += [0] * (want * 3 - len(pal))
    idx = list(pal_img.getdata())

    if has_alpha:
        palette = [(0, 0, 0)] + [tuple(pal[i * 3:i * 3 + 3]) for i in range(want)]
        alpha = list(rgba.split()[3].getdata())
        idx = [0 if a < 128 else v + 1 for v, a in zip(idx, alpha)]
    else:
        palette = [tuple(pal[i * 3:i * 3 + 3]) for i in range(want)]

    hdr = bytearray(64)
    hdr[0:4] = b"CIM1"
    hdr[4], hdr[5] = w & 255, w >> 8
    hdr[6], hdr[7] = h & 255, h >> 8
    for i in range(16):
        entry = palette[i] if i < len(palette) else (0, 0, 0)
        r, g, b = (tuple(entry) + (0, 0, 0))[:3]
        hdr[16 + i * 3] = r
        hdr[17 + i * 3] = g
        hdr[18 + i * 3] = b

    rows = bytearray()
    for y in range(h):
        row = bytearray((w + 1) // 2)
        for x in range(w):
            v = idx[y * w + x] & 15
            if x & 1:
                row[x >> 1] |= v << 4
            else:
                row[x >> 1] |= v
        rows += row

    with open(dst, "wb") as f:
        f.write(hdr)
        f.write(rows)
    print("%s: %dx%d, %d bytes%s"
          % (dst, w, h, 64 + len(rows), ", index 0 transparent" if has_alpha else ""))


if __name__ == "__main__":
    main()
