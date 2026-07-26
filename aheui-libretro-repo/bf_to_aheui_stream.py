"""
bf_to_aheui_stream.py — streams BF -> pure Hangul Aheui grid to a file.

Same scaffold and same verified leaf blocks as bf_to_aheui.py, but rows are
written out as they are produced (the loop frames don't need to know the
body's size in advance, since the bypass lanes are vertical columns and the
landing rows handle convergence). Rows are stored right-stripped; the runner
re-pads them to a common width on load, which is required for lane traversal
but is pure waste on disk.
"""

import bf_to_aheui as C

BLOCKS = {
    '+': C.BLOCK_PLUS, '-': C.BLOCK_MINUS, '.': C.BLOCK_OUT,
    ',': C.BLOCK_IN,   '>': C.BLOCK_RIGHT, '<': C.BLOCK_LEFT,
}


class Stream:
    def __init__(self, out):
        self.out = out
        self.rows = 0
        self.width = 0

    def row(self, r):
        r = r.rstrip()
        if len(r) > self.width:
            self.width = len(r)
        self.out.write(r + '\n')
        self.rows += 1


def _match(tokens):
    """Precompute bracket partners once (the original rescanned per loop)."""
    m, st = {}, []
    for i, c in enumerate(tokens):
        if c == '[':
            st.append(i)
        elif c == ']':
            j = st.pop(); m[j] = i; m[i] = j
    return m


def compile_stream(tokens, s, match, lo, hi, depth):
    i = lo
    while i < hi:
        c = tokens[i]
        if c in BLOCKS:
            for r in BLOCKS[c]:
                s.row(r)
        elif c == '[':
            j = match[i]
            SKIP = 8 + 2 * depth
            BACK = SKIP + 1
            # entry frame
            s.row(C.DUP_SKIP2)
            s.row(C.set_char(C.NOOP_RIGHT, SKIP, C.NOOP_DOWN))
            s.row(C.CHECK_DOWN)
            s.row(C.set_char(C.NOOP_DOWN, BACK, C.NOOP_LEFT))
            # body
            if j == i + 1:
                s.row(C.NOOP_DOWN)          # degenerate empty body
            else:
                compile_stream(tokens, s, match, i + 1, j, depth + 1)
            # exit frame
            s.row(C.DUP_SKIP2)
            s.row(C.set_char(C.NOOP_RIGHT, BACK, C.NOOP_UP))
            s.row(C.CHECK_UP)
            s.row(C.set_char(C.NOOP_DOWN, SKIP, C.NOOP_LEFT))
            i = j
        i += 1


def compile_to_file(src, out):
    tokens = [c for c in src if c in '+-.,<>[]']
    s = Stream(out)
    s.row(C.BLOCK_BEGIN[0])
    compile_stream(tokens, s, _match(tokens), 0, len(tokens), 0)
    s.row(C.BLOCK_HALT[0])
    return s.rows, s.width


def load_grid(text):
    """Re-pad a stored ragged grid back to a rectangle for execution."""
    rows = text.split('\n')
    if rows and rows[-1] == '':
        rows.pop()
    w = max(len(r) for r in rows)
    return '\n'.join(r + ' ' * (w - len(r)) for r in rows)
