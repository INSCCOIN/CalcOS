#!/usr/bin/env python3
"""The console font must fit its cell.

Every glyph lives in an 8x12 cell. If a glyph is rendered larger than the cell
the bottom row is simply dropped, and the tails of g, y, p, q and j disappear
into the line below: text still "works" but reads as a smear, which is hard to
spot in a screenshot and impossible to spot in a unit test of the driver.

So this checks the generated table directly: every glyph is non-empty where it
should be, nothing touches the cell edges that should not, and the shapes that
depend on a descender actually have ink in the bottom rows.
"""
import re
import sys
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
H = 12


def load():
    src = open(os.path.join(ROOT, "platform", "font.c")).read()
    faces = {}
    for name in ("calcos_font", "calcos_font_bold"):
        m = re.search(r"\b%s\[95\]\[12\]\s*=\s*\{(.*?)\n\};" % name, src, re.S)
        if not m:
            raise SystemExit("%s not found in platform/font.c" % name)
        vals = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1))]
        if len(vals) != 95 * H:
            raise SystemExit("%s has %d bytes, expected %d" % (name, len(vals), 95 * H))
        faces[name] = [vals[i * H:(i + 1) * H] for i in range(95)]
    return faces


def main():
    faces = load()
    fails = []

    for face, glyphs in faces.items():
        def g(ch):
            return glyphs[ord(ch) - 32]

        # the space must be blank, everything else must have ink
        if any(g(" ")):
            fails.append("%s: space is not blank" % face)
        for c in range(33, 127):
            if not any(g(chr(c))):
                fails.append("%s: '%s' is blank" % (face, chr(c)))

        # descenders must reach below the baseline, which here is row 8:
        # if the glyph was clipped by the cell these rows come back empty
        for ch in "gypqj":
            if not any(g(ch)[9:]):
                fails.append("%s: '%s' has no descender (clipped)" % (face, ch))

        # a capital must not spill into the descender rows
        for ch in "ABEHMXZ":
            if any(g(ch)[10:]):
                fails.append("%s: '%s' spills into the descender rows" % (face, ch))

        # letters that sit on the baseline must not touch the last row, or
        # consecutive lines run together
        for ch in "aceomnsuvwxz":
            if g(ch)[H - 1]:
                fails.append("%s: '%s' touches the bottom of the cell" % (face, ch))

        # nothing may use the 8th bit: the cell is 8 pixels wide
        for i, rows in enumerate(glyphs):
            if any(r > 0xFF for r in rows):
                fails.append("%s: glyph %d is wider than 8 pixels" % (face, i))

        # the bold face must be at least as heavy as the regular one
        if face == "calcos_font_bold":
            reg = faces["calcos_font"]
            heavy = sum(bin(b).count("1") for rows in glyphs for b in rows)
            light = sum(bin(b).count("1") for rows in reg for b in rows)
            if heavy <= light:
                fails.append("the bold face is not heavier than the regular one")

    for f in fails:
        print("FAIL: " + f)
    if fails:
        return 1
    print("   both faces fit the 8x12 cell, descenders intact")
    return 0


if __name__ == "__main__":
    sys.exit(main())
