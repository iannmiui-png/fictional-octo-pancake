SP = ' '
BASE = 0xAC00

def compose(cho, ju, jo=0):
    return chr(BASE + (cho*21 + ju)*28 + jo)

# scaffold cells (choseong index, jungseong index) - all verified against the
# same op table as the real Aheui.lua interpreter
DUP_SKIP2  = compose(8, 17)   # ㅃ dup, jung=ㅠ (double-step down)  -> hop over 1 row
NOOP_RIGHT = compose(11, 0)   # ㅇ noop, jung=ㅏ (right)
NOOP_LEFT  = compose(11, 4)   # ㅇ noop, jung=ㅓ (left)
NOOP_DOWN  = compose(11, 13)  # ㅇ noop, jung=ㅜ (down)
NOOP_UP    = compose(11, 8)   # ㅇ noop, jung=ㅗ (up)
CHECK_DOWN = compose(14, 13)  # ㅊ cond,  jung=ㅜ (down baseline; zero -> flips to up)
CHECK_UP   = compose(14, 8)   # ㅊ cond,  jung=ㅗ (up baseline;  zero -> flips to down)

# verified leaf blocks (from earlier simulation work)
BLOCK_PLUS  = ['발발나다붗', '루떠떠벓벓']
BLOCK_MINUS = ['밟밠밥따따받두', '루떠떠벓벓벝더']
BLOCK_OUT   = ['뿌', '뭏']
# discard old cell (no-print pop), then push the new input char.
# Both syllables must use jungseong ㅜ (down) so the block enters at column 0
# moving down and exits at column 0 moving down, like every other leaf block.
# The old one-row form ['마밯'] used ㅏ (right), so control ran off the end of
# the row, wrapped back to column 0 of the SAME row, and looped forever --
# every program containing ',' hung.
BLOCK_IN    = ['무', '붛']
BLOCK_RIGHT = ['싹순', '수빠쑤', '부수머', '우어']
BLOCK_LEFT  = ['싼숙', '수빠쑤', '부수머', '우어']
BLOCK_HALT  = ['희']
BLOCK_BEGIN = ['부']


def set_char(row, col, ch):
    if len(row) <= col:
        row = row + SP * (col - len(row) + 1)
    return row[:col] + ch + row[col+1:]


class Compiler:
    def __init__(self):
        self.rows = list(BLOCK_BEGIN)

    def emit(self, block_rows):
        self.rows.extend(block_rows)

    def compile_tokens(self, tokens, depth):
        i = 0
        while i < len(tokens):
            c = tokens[i]
            if c == '+': self.emit(BLOCK_PLUS)
            elif c == '-': self.emit(BLOCK_MINUS)
            elif c == '.': self.emit(BLOCK_OUT)
            elif c == ',': self.emit(BLOCK_IN)
            elif c == '>': self.emit(BLOCK_RIGHT)
            elif c == '<': self.emit(BLOCK_LEFT)
            elif c == '[':
                depth_ct, j = 1, i + 1
                while j < len(tokens) and depth_ct > 0:
                    if tokens[j] == '[': depth_ct += 1
                    elif tokens[j] == ']': depth_ct -= 1
                    j += 1
                self.compile_loop(tokens[i+1:j-1], depth)
                i = j - 1
            i += 1

    def compile_loop(self, body_tokens, depth):
        # Lane columns only need to clear the widest leaf block (7 cols) and
        # stay distinct per nesting depth, with the outer lanes strictly left
        # of the inner ones so an outer lane always finds blank cells while
        # gliding through inner frame rows. Two columns per depth suffices.
        # (These were 100+80*depth / +40, which padded every row of the grid
        # out to column 540 -- 4.4 GB for the full game.)
        SKIP_COL = 8 + 2 * depth
        BACK_COL = SKIP_COL + 1

        # --- entry frame ---
        self.rows.append(DUP_SKIP2)                                   # R0
        r1 = set_char(NOOP_RIGHT, SKIP_COL, NOOP_DOWN)                 # R1 (turn: skip lane)
        self.rows.append(r1)
        self.rows.append(CHECK_DOWN)                                  # R2 (entry check)

        # dedicated blank landing row: BOTH the normal fall-through (down from
        # R2) and the repeat/back lane (arriving left along BACK_COL) pass
        # through here safely, since this row has no real block content for
        # a glide to accidentally trigger - only col0 (down) and BACK_COL (left)
        r_land_in = set_char(NOOP_DOWN, BACK_COL, NOOP_LEFT)
        self.rows.append(r_land_in)                                   # R2.5

        # --- body ---
        body_compiler = Compiler.__new__(Compiler)
        body_compiler.rows = []
        body_compiler.compile_tokens(body_tokens, depth + 1)
        body_rows = body_compiler.rows
        if not body_rows:
            body_rows = [NOOP_DOWN]  # degenerate empty body safety net
        self.rows.extend(body_rows)

        # --- exit frame ---
        self.rows.append(DUP_SKIP2)                                    # R(k+1)
        r_turnback = set_char(NOOP_RIGHT, BACK_COL, NOOP_UP)            # R(k+2)
        self.rows.append(r_turnback)
        self.rows.append(CHECK_UP)                                     # R(k+3)

        # dedicated blank landing row after the loop: the natural "stop" path
        # (falls straight down from the exit check) and the skip lane (arrives
        # left along SKIP_COL) both converge here safely before continuing on
        # to whatever real instruction comes next.
        r_land_out = set_char(NOOP_DOWN, SKIP_COL, NOOP_LEFT)
        self.rows.append(r_land_out)                                   # R(k+4)

    def finish(self):
        self.rows.append(BLOCK_HALT[0])
        width = max(len(r) for r in self.rows)
        return [r + SP * (width - len(r)) for r in self.rows]


def compile_bf(src):
    tokens = [c for c in src if c in '+-.,<>[]']
    comp = Compiler()
    comp.compile_tokens(tokens, 0)
    return comp.finish()
