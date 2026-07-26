#!/usr/bin/env python3
"""
maze_bf.py [options]

Emits a Brainfuck program that computes and prints a maze at run time.

The maze is a binary-tree maze -- each cell carves either south or east -- with
the carve choice taken from a Rule 110 cellular automaton, then perturbed by
the *next* Rule 110 generation: where the perturbation bit is set the cell
opens both ways, braiding loops into what would otherwise be a perfect maze.
Nothing is precomputed. The CA transitions run in Brainfuck cells; only the
loops over x and y are unrolled, which is what makes the border cases free.

Rendering is two lines per maze row:

    +---+   +      node, then '-' if you can go east
    |   .   |      '|' if you can go south, then a speckle cell

so the pipes alone tell you every direction you can leave a cell by. Where the
noise bit fires the node becomes an arrow along a direction that is actually
open -- a hint, never a lie. The dead column beside each '|' takes the
low-codepage speckle, which litters the gaps without touching the maze.

Byte set: --cp437 (default) emits the DOS bytes -- 0xC5 0xC4 0xB3 for the pipes, 0x1A/0x19
for the arrows, 0xB0/0xB1 for the shading. --ascii stays inside the printable
range instead, for a terminal with no CP437 font.

Tape layout: every maze column owns a block of STRIDE cells holding its own CA
state and its own scratch. An earlier version kept one shared scratch block
near address 0, so each copy emitted a 30-cell pointer run and the 16x16 maze
compiled to 921k instructions. Locality is worth about 9x here.

Options:
  --w N --h N     maze size in cells      (default 16 x 16)
  --seed HEX      initial CA row bitmask
  --cp437         DOS bytes instead of printable ASCII
  --out FILE      where to write the .b   (default maze.b)
"""
import sys

STRIDE = 11
REG = {'ca': 0, 'new': 1, 'E': 2, 'S': 3, 'TMP': 4, 'TMP2': 5,
       'N': 6, 'FLAG': 7, 'CP': 8, 'K': 9, 'OUT': 10}
COL0 = 0                                # column -1, the left guard, starts here


class BF:
    """Emitter that tracks the pointer, so every address is absolute.
    `k` is the scratch cell setc() may clobber; the caller keeps it local."""

    def __init__(self):
        self.out = []
        self.p = 0
        self.k = None

    def code(self):
        return ''.join(self.out)

    def at(self, a):
        d = a - self.p
        if d:
            self.out.append(('>' if d > 0 else '<') * abs(d))
            self.p = a

    def raw(self, s):
        self.out.append(s)

    def zero(self, a):
        self.at(a); self.raw('[-]')

    def inc(self, a, n):
        if n:
            self.at(a); self.raw(('+' if n > 0 else '-') * abs(n))

    def setc(self, a, n):
        self.zero(a)
        n = (n if isinstance(n, int) else ord(n)) & 255
        if n <= 16:
            self.inc(a, n); return
        q, r = divmod(n, 10)
        self.zero(self.k); self.inc(self.k, 10)
        self.at(self.k); self.raw('[')
        self.inc(a, q)
        self.at(self.k); self.raw('-')
        self.raw(']')
        self.inc(a, r)

    def move(self, src, dst):
        self.at(src); self.raw('[')
        self.at(dst); self.raw('+')
        self.at(src); self.raw('-')
        self.raw(']')

    def copy(self, src, dst, tmp):
        self.zero(dst); self.zero(tmp)
        self.at(src); self.raw('[')
        self.at(dst); self.raw('+')
        self.at(tmp); self.raw('+')
        self.at(src); self.raw('-')
        self.raw(']')
        self.move(tmp, src)

    def if_nz(self, a, body):
        """Run body once if a is nonzero, then clear a. The pointer returns to
        a before the ']' so the loop closes where it opened."""
        self.at(a); self.raw('[')
        body()
        self.at(a); self.raw('[-]')
        self.raw(']')

    def emit(self, a, ch):
        self.setc(a, ch)
        self.at(a); self.raw('.')


GLYPH_ASCII = {'node': '+', 'h': '-', 'v': '|', 'blank': ' ',
               'east': '>', 'south': 'v', 'dust': ('.', ':')}
GLYPH_CP437 = {'node': 0xC5, 'h': 0xC4, 'v': 0xB3, 'blank': 0x20,
               'east': 0x1A, 'south': 0x19, 'dust': (0xB0, 0xB1)}

NOISE_SHIFT = (1, 5, 2)                 # arrow, speckle-on, speckle-pick


def default_seed(w):
    """Scattered ones. Rule 110 from a single cell stays regular for a long
    time, which makes a boring maze."""
    s = 0
    for i in (0, 3, 4, 9, 13, 14):
        if i < w:
            s |= 1 << i
    return s


class Maze:
    def __init__(self, w=16, h=16, seed=None, cp437=False):
        self.w, self.h = w, h
        self.cp437 = cp437
        self.seed = default_seed(w) if seed is None else seed
        self.g = GLYPH_CP437 if cp437 else GLYPH_ASCII

    def r(self, x, name):
        """Register `name` of column x. x runs -1..w; the two extra columns are
        guards whose ca stays 0, which is the CA's boundary condition."""
        return COL0 + STRIDE * (x + 1) + REG[name]

    def col(self, bf, x):
        bf.k = self.r(x, 'K')

    # ── Rule 110: out = (c OR r) AND NOT (l AND c AND r)
    def rule110(self, bf, x):
        self.col(bf, x)
        l, c, r = self.r(x - 1, 'ca'), self.r(x, 'ca'), self.r(x + 1, 'ca')
        o = self.r(x, 'new')
        E, S = self.r(x, 'E'), self.r(x, 'S')
        TMP, TMP2 = self.r(x, 'TMP'), self.r(x, 'TMP2')
        N, FLAG, CP = self.r(x, 'N'), self.r(x, 'FLAG'), self.r(x, 'CP')

        bf.zero(E)                                       # E = c OR r
        bf.copy(c, TMP, CP)
        bf.if_nz(TMP, lambda: bf.setc(E, 1))
        bf.copy(r, TMP, CP)
        bf.if_nz(TMP, lambda: bf.setc(E, 1))

        bf.zero(FLAG)                                    # FLAG = l AND c AND r
        bf.copy(l, S, CP)

        def lvl2():
            bf.copy(c, N, CP)

            def lvl3():
                bf.copy(r, TMP2, CP)
                bf.if_nz(TMP2, lambda: bf.setc(FLAG, 1))
            bf.if_nz(N, lvl3)
        bf.if_nz(S, lvl2)

        bf.zero(o)
        bf.move(E, o)
        bf.if_nz(FLAG, lambda: bf.zero(o))

    def step(self, bf):
        for x in range(self.w):
            self.rule110(bf, x)

    def promote(self, bf):
        for x in range(self.w):
            bf.zero(self.r(x, 'ca'))
            bf.move(self.r(x, 'new'), self.r(x, 'ca'))

    # ── a cell's exits. a = carve bit, b = perturbation bit.
    #    east = (NOT a) OR b,  south = a OR b
    # Borders resolve at generation time, so an edge cell costs two constants.
    def exits(self, bf, x, y):
        self.col(bf, x)
        E, S = self.r(x, 'E'), self.r(x, 'S')
        TMP, CP = self.r(x, 'TMP'), self.r(x, 'CP')
        last_x, last_y = x == self.w - 1, y == self.h - 1

        if last_x and last_y:
            bf.setc(E, 0); bf.setc(S, 0); return
        if last_x:                                       # right edge: go south
            bf.setc(E, 0); bf.setc(S, 1); return
        if last_y:                                       # bottom edge: go east
            bf.setc(E, 1); bf.setc(S, 0); return

        a, b = self.r(x, 'ca'), self.r(x, 'new')
        bf.setc(E, 1)
        bf.copy(a, TMP, CP)
        bf.if_nz(TMP, lambda: bf.zero(E))                # east = NOT a
        bf.setc(S, 0)
        bf.copy(a, TMP, CP)
        bf.if_nz(TMP, lambda: bf.setc(S, 1))             # south = a
        bf.copy(b, TMP, CP)
        bf.if_nz(TMP, lambda: (bf.setc(E, 1), bf.setc(S, 1)))    # ... OR b

    def noise(self, bf, x, shift, dst):
        src = self.r((x + shift) % self.w, 'new')
        bf.copy(src, dst, self.r(x, 'CP'))

    def line_a(self, bf):
        g = self.g
        for x in range(self.w):
            self.col(bf, x)
            E, S = self.r(x, 'E'), self.r(x, 'S')
            TMP, TMP2 = self.r(x, 'TMP'), self.r(x, 'TMP2')
            N, FLAG, CP = self.r(x, 'N'), self.r(x, 'FLAG'), self.r(x, 'CP')
            OUT = self.r(x, 'OUT')

            bf.setc(OUT, g['node'])
            bf.setc(FLAG, 1)                             # 1 = no arrow yet
            self.noise(bf, x, NOISE_SHIFT[0], N)

            def arrow():
                bf.copy(E, TMP2, CP)

                def east():
                    bf.setc(OUT, g['east'])
                    bf.zero(FLAG)
                bf.if_nz(TMP2, east)

                bf.copy(FLAG, TMP2, CP)                  # only if east was shut

                def south():
                    bf.copy(S, TMP, CP)
                    bf.if_nz(TMP, lambda: bf.setc(OUT, g['south']))
                bf.if_nz(TMP2, south)
            bf.if_nz(N, arrow)
            bf.at(OUT); bf.raw('.')

            bf.setc(OUT, g['blank'])                     # the east link
            bf.copy(E, TMP2, CP)
            bf.if_nz(TMP2, lambda: bf.setc(OUT, g['h']))
            bf.at(OUT); bf.raw('.')
        self.col(bf, 0)
        bf.emit(self.r(0, 'OUT'), 10)

    def line_b(self, bf):
        g = self.g
        for x in range(self.w):
            self.col(bf, x)
            S = self.r(x, 'S')
            TMP2, N, CP = self.r(x, 'TMP2'), self.r(x, 'N'), self.r(x, 'CP')
            OUT = self.r(x, 'OUT')

            bf.setc(OUT, g['blank'])
            bf.copy(S, TMP2, CP)
            bf.if_nz(TMP2, lambda: bf.setc(OUT, g['v']))
            bf.at(OUT); bf.raw('.')

            bf.setc(OUT, g['blank'])                     # speckle in dead space
            self.noise(bf, x, NOISE_SHIFT[1], N)

            def speckle():
                bf.setc(OUT, g['dust'][0])
                self.noise(bf, x, NOISE_SHIFT[2], TMP2)
                bf.if_nz(TMP2, lambda: bf.setc(OUT, g['dust'][1]))
            bf.if_nz(N, speckle)
            bf.at(OUT); bf.raw('.')
        self.col(bf, 0)
        bf.emit(self.r(0, 'OUT'), 10)

    def build(self):
        bf = BF()
        for x in range(self.w):                          # seed the CA row
            self.col(bf, x)
            if (self.seed >> x) & 1:
                bf.setc(self.r(x, 'ca'), 1)
        for y in range(self.h):
            self.step(bf)                                # the perturbing gen
            for x in range(self.w):
                self.exits(bf, x, y)                     # once, not per line
            self.line_a(bf)
            self.line_b(bf)
            self.promote(bf)
        return bf.code()


# ── An independent model of the same maze, written from the description at the
# top rather than from the emitter, so agreement between the two is evidence
# rather than a tautology.
def model(w=16, h=16, seed=None, cp437=True):   # default must track Maze's
    g = GLYPH_CP437 if cp437 else GLYPH_ASCII
    ch = lambda v: chr(v) if isinstance(v, int) else v
    seed = default_seed(w) if seed is None else seed
    ca = [0] * (w + 2)
    for i in range(w):
        if (seed >> i) & 1:
            ca[i + 1] = 1
    lines = []
    for y in range(h):
        new = [0] * (w + 2)
        for i in range(1, w + 1):
            l, c, r = ca[i - 1], ca[i], ca[i + 1]
            new[i] = 1 if ((c or r) and not (l and c and r)) else 0
        A = B = ''
        for x in range(w):
            if x == w - 1 and y == h - 1:   e, s = 0, 0
            elif x == w - 1:                e, s = 0, 1
            elif y == h - 1:                e, s = 1, 0
            else:
                a, b = ca[x + 1], new[x + 1]
                e, s = (0 if a else 1), a
                if b:
                    e = s = 1
            node = g['node']
            if new[((x + NOISE_SHIFT[0]) % w) + 1]:
                if e:     node = g['east']
                elif s:   node = g['south']
            A += ch(node) + ch(g['h'] if e else g['blank'])
            B += ch(g['v'] if s else g['blank'])
            if new[((x + NOISE_SHIFT[1]) % w) + 1]:
                B += ch(g['dust'][1 if new[((x + NOISE_SHIFT[2]) % w) + 1] else 0])
            else:
                B += ch(g['blank'])
        lines += [A, B]
        ca = new
    return '\n'.join(lines) + '\n'


def main(argv):
    w = h = 16
    seed = None
    cp437 = True
    out = 'maze.b'
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == '--w': i += 1; w = int(argv[i])
        elif a == '--h': i += 1; h = int(argv[i])
        elif a == '--seed': i += 1; seed = int(argv[i], 0)
        elif a == '--cp437': cp437 = True
        elif a == '--ascii': cp437 = False
        elif a == '--out': i += 1; out = argv[i]
        elif a in ('-h', '--help'): print(__doc__); return 0
        else: print(f'unknown option {a}'); return 1
        i += 1

    code = Maze(w, h, seed, cp437).build()
    open(out, 'w').write(code)
    ops = sum(code.count(c) for c in '+-.,<>[]')
    print(f'{out}: {ops:,} Brainfuck instructions, {w}x{h}, '
          f'{"CP437" if cp437 else "ASCII"}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
