#!/usr/bin/env python3
"""
make_font.py [out.h]

Bakes an 8x8 CP437 font into a C header.

ASCII comes from a monospace TTF, thresholded into the 8x8 cell. The box
drawing, block and shading glyphs are drawn procedurally instead: at 8 pixels
a rasteriser puts the stems wherever antialiasing happens to land, and box
drawing only looks right if every glyph puts its stem on exactly the same row
and column so neighbouring cells join up. Arrows are drawn too, since the CP437
ones live at 0x18-0x1B where a TTF has control characters.
"""
import sys

from PIL import Image, ImageDraw, ImageFont

W = H = 8
MID_X, MID_Y = 3, 3          # the row and column every line glyph shares
# Bold at 11px, thresholded into 8x8: more ink survives the threshold, and a
# search over face/size/threshold showed it is the only combination tried that
# leaves no two printable ASCII characters sharing a bitmap. Regular rendered
# '8' and 'B' identically, which is a misreading on screen and not only in a
# test.
FONTS = ['/usr/share/fonts/truetype/liberation/LiberationMono-Bold.ttf',
         '/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf',
         '/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf']
THRESH = 110


def blank():
    return [[0] * W for _ in range(H)]


def hline(g, x0, x1, y=MID_Y):
    for x in range(x0, x1 + 1):
        g[y][x] = 1


def vline(g, y0, y1, x=MID_X):
    for y in range(y0, y1 + 1):
        g[y][x] = 1


def box(dirs, double=False):
    """dirs: which of n/e/s/w the glyph reaches toward."""
    g = blank()
    off = 2 if double else 0
    for d in dirs:
        if d == 'n': vline(g, 0, MID_Y)
        if d == 's': vline(g, MID_Y, H - 1)
        if d == 'w': hline(g, 0, MID_X)
        if d == 'e': hline(g, MID_X, W - 1)
    if double:                                  # a second stem two cells over
        for d in dirs:
            if d == 'n': vline(g, 0, MID_Y + off, MID_X + off)
            if d == 's': vline(g, MID_Y, H - 1, MID_X + off)
            if d == 'w': hline(g, 0, MID_X + off, MID_Y + off)
            if d == 'e': hline(g, MID_X, W - 1, MID_Y + off)
    return g


def shade(density):
    """Ordered dither, so 25/50/75% tile without seams."""
    g = blank()
    for y in range(H):
        for x in range(W):
            if density == 1:   on = (x % 4 == 0) and (y % 2 == 0)
            elif density == 2: on = (x + y) % 2 == 0
            elif density == 3: on = not ((x % 4 == 2) and (y % 2 == 1))
            else:              on = True
            g[y][x] = 1 if on else 0
    return g


def half(where):
    g = blank()
    for y in range(H):
        for x in range(W):
            if where == 'lower' and y >= H // 2: g[y][x] = 1
            if where == 'upper' and y < H // 2:  g[y][x] = 1
            if where == 'left' and x < W // 2:   g[y][x] = 1
            if where == 'right' and x >= W // 2: g[y][x] = 1
    return g


def arrow(d):
    g = blank()
    if d in 'ud':
        vline(g, 1, H - 2, MID_X)
        for k in range(3):
            y = 1 + k if d == 'u' else H - 2 - k
            for x in range(MID_X - k, MID_X + k + 1):
                if 0 <= x < W: g[y][x] = 1
    else:
        hline(g, 1, W - 2, MID_Y)
        for k in range(3):
            x = 1 + k if d == 'l' else W - 2 - k
            for y in range(MID_Y - k, MID_Y + k + 1):
                if 0 <= y < H: g[y][x] = 1
    return g


def triangle(d):
    """Solid triangle. CP437 keeps these separate from the shafted arrows --
    0x10 is a triangle, 0x1A is an arrow with a stem -- and if they share a
    glyph they are indistinguishable on screen, not merely in a test."""
    g = blank()
    for i in range(4):
        span = 3 - i
        for k in range(-span, span + 1):
            if   d == 'r': x, y = 1 + i, MID_Y + k
            elif d == 'l': x, y = 6 - i, MID_Y + k
            elif d == 'd': x, y = MID_X + k, 1 + i
            else:          x, y = MID_X + k, 6 - i
            if 0 <= x < W and 0 <= y < H:
                g[y][x] = 1
    return g


def solid_small(r=2):
    g = blank()
    for y in range(4 - r, 4 + r):
        for x in range(4 - r, 4 + r):
            g[y][x] = 1
    return g


# CP437 line-drawing assignments, single and double stems
BOXES = {
    0xB3: ('ns', 0), 0xB4: ('nsw', 0), 0xB9: ('nsw', 1), 0xBA: ('ns', 1),
    0xBB: ('sw', 1), 0xBC: ('nw', 1), 0xBF: ('sw', 0), 0xC0: ('ne', 0),
    0xC1: ('new', 0), 0xC2: ('esw', 0), 0xC3: ('nse', 0), 0xC4: ('ew', 0),
    0xC5: ('nsew', 0), 0xC8: ('ne', 1), 0xC9: ('se', 1), 0xCA: ('new', 1),
    0xCB: ('esw', 1), 0xCC: ('nse', 1), 0xCD: ('ew', 1), 0xCE: ('nsew', 1),
    0xD9: ('nw', 0), 0xDA: ('se', 0),
}


def procedural():
    out = {}
    for code, (dirs, dbl) in BOXES.items():
        out[code] = box(dirs, bool(dbl))
    out[0xB0], out[0xB1], out[0xB2] = shade(1), shade(2), shade(3)
    out[0xDB] = shade(4)
    out[0xDC], out[0xDF] = half('lower'), half('upper')
    out[0xDD], out[0xDE] = half('left'), half('right')
    out[0xFE] = solid_small(2)
    out[0x18], out[0x19] = arrow('u'), arrow('d')
    out[0x1A], out[0x1B] = arrow('r'), arrow('l')
    out[0x1E], out[0x1F] = triangle('u'), triangle('d')
    out[0x10], out[0x11] = triangle('r'), triangle('l')
    out[0x07] = solid_small(1)      # bullet, smaller than the 0xFE square
    return out


LOW = (' \u263A\u263B\u2665\u2666\u2663\u2660\u2022\u25D8\u25CB\u25D9\u2642'
       '\u2640\u266A\u266B\u263C\u25BA\u25C4\u2195\u203C\u00B6\u00A7\u25AC'
       '\u21A8\u2191\u2193\u2192\u2190\u221F\u2194\u25B2\u25BC')


def unicode_for(code):
    if code < 32:
        return LOW[code]
    if code < 127:
        return chr(code)
    if code == 127:
        return '\u2302'
    return bytes([code]).decode('cp437')


def render_ttf(path, size, ch):
    img = Image.new('L', (W, H), 0)
    d = ImageDraw.Draw(img)
    f = ImageFont.truetype(path, size)
    try:
        bb = d.textbbox((0, 0), ch, font=f)
    except Exception:
        return None
    x = (W - (bb[2] - bb[0])) // 2 - bb[0]
    y = (H - (bb[3] - bb[1])) // 2 - bb[1]
    d.text((x, y), ch, font=f, fill=255)
    px = img.load()
    return [[1 if px[x, y] >= THRESH else 0 for x in range(W)] for y in range(H)]


def show(g):
    return '\n'.join(''.join('#' if v else '.' for v in row) for row in g)


def main(out='font8x8.h', size=11):
    path = next((p for p in FONTS if __import__('os').path.exists(p)), None)
    if not path:
        raise SystemExit('no monospace TTF found')
    proc = procedural()
    glyphs = []
    for code in range(256):
        if code in proc:
            glyphs.append(proc[code])
            continue
        ch = unicode_for(code)
        g = render_ttf(path, size, ch) if ch.strip() else blank()
        glyphs.append(g or blank())

    with open(out, 'w') as f:
        f.write('/* Generated by make_font.py -- do not edit.\n'
                ' * 8x8 CP437 font. ASCII rendered from %s;\n'
                ' * line, block, shade and arrow glyphs drawn procedurally so\n'
                ' * that adjacent cells join without seams. */\n'
                '#ifndef FONT8X8_H\n#define FONT8X8_H\n\n'
                'static const unsigned char font8x8[256][8] = {\n'
                % path.rsplit('/', 1)[-1])
        for code, g in enumerate(glyphs):
            row = ','.join('0x%02X' % sum(b << i for i, b in enumerate(g[y]))
                           for y in range(H))
            f.write('  {%s}, /* %02X */\n' % (row, code))
        f.write('};\n\n')
        # The core needs the byte -> Unicode direction too, to turn a typed
        # character back into the byte a program will read.
        f.write('/* CP437 byte -> Unicode, the graphics reading of 0x00-0x1F\n'
                ' * included, matching the font above. */\n'
                'static const unsigned short cp437_unicode[256] = {\n')
        for base in range(0, 256, 8):
            f.write('  ' + ', '.join('0x%04X' % ord(unicode_for(c))
                                     for c in range(base, base + 8)) + ',\n')
        f.write('};\n\n#endif\n')
    print(f'{out}: 256 glyphs from {path.rsplit("/", 1)[-1]}')
    dup = {}
    for i, g in enumerate(glyphs):
        k = tuple(tuple(r) for r in g)
        if any(any(r) for r in g):
            dup.setdefault(k, []).append(i)
    clash = [v for v in dup.values() if len(v) > 1]
    print('glyphs sharing a bitmap:',
          [' '.join('%02X' % c for c in v) for v in clash] or 'none')

    for c in (0xC5, 0xC4, 0xB1, 0x1A, 0x10, ord('A'), ord('g')):
        print(f'\n0x{c:02X}:'); print(show(glyphs[c]))


if __name__ == '__main__':
    main(*(sys.argv[1:] or []))
