#!/usr/bin/env python3
"""
test_core.py

Runs content through the libretro core via test_frontend and checks what
appears on screen against the references the rest of the toolchain already
agrees on: the maze's independent model, and the known text of the two games.

The screen is read back from the framebuffer by glyph matching, so a pass here
covers the PNG decoder, the inflate, the VM, the terminal and the renderer.
What it does NOT cover is the libretro ABI itself -- both the core and this
frontend are built against the same libretro.h in this directory, so they
would agree even if a constant in it disagreed with RetroArch.
"""
import subprocess
import sys

import maze_bf as M
from maze_test import cp437_table

# Python's cp437 codec maps 0x00-0x1F to C0 controls. The graphics reading of
# that range -- where 0x1A is an arrow -- is the one the core, the console and
# maze_bf all use, so the test has to read the screen the same way.
CP437 = cp437_table()


def glyph_classes(path='font8x8.h'):
    """Codes that share a bitmap are indistinguishable on screen, so reading
    text back from pixels cannot separate them -- 0x7C '|' and 0xB3 '│' are
    the same eight bytes. Fold each class onto one representative on both
    sides of a comparison. The classes are derived from the font rather than
    listed here, and make_font.py prints them, so a new collision shows up as
    a report rather than as a silently loosened test."""
    rows, cur = [], []
    for line in open(path):
        line = line.strip()
        if not line.startswith('{0x'):
            continue
        cur = tuple(int(v, 16) for v in
                    line[1:line.index('}')].split(','))
        rows.append(cur)
    rep, seen = {}, {}
    for code, bits in enumerate(rows):
        if bits in seen:
            rep[code] = seen[bits]
        else:
            seen[bits] = code
            rep[code] = code
    return rep


REP = glyph_classes()


def canon(s):
    return ''.join(CP437[REP.get(_ord(c), _ord(c))] for c in s)


def _ord(c):
    i = CP437.index(c) if c in CP437 else ord(c)
    return i & 255

CORE = './aheui_libretro.so'
FRONT = './test_frontend'


def screen(content, frames=600, typed=(), speed='fast'):
    cmd = [FRONT, CORE, content, '--frames', str(frames), '--speed', speed]
    for t in typed:
        cmd += ['--type', t]
    r = subprocess.run(cmd, capture_output=True)
    txt = ''.join(CP437[b] if b != 10 else '\n' for b in r.stdout)
    lines = txt.split('\n')
    # the block cursor is drawn over the terminal, not into it
    lines = [l.rstrip('\u2588') for l in lines]
    return [l for l in lines if l.strip()], r.stderr.decode('utf-8', 'replace')


def check(name, ok, detail=''):
    print(('PASS ' if ok else 'FAIL ') + name + (('  ' + detail) if detail else ''))
    return ok


def main():
    allok = True

    got, err = screen('hello.png', frames=30)
    allok &= check('hello.png', got and got[0] == 'Hello World!', repr(got[:1]))

    got, _ = screen('hello.b', frames=60)
    allok &= check('hello.b (core compiles Brainfuck)',
                   bool(got) and got[0] == 'Hello World!', repr(got[:1]))

    got, _ = screen('hand.aheui', frames=30)
    allok &= check('hand.aheui (out-of-alphabet grid as text)',
                   bool(got) and got[0] == '9', repr(got[:1]))

    # the frontend trims trailing blanks off each screen line, so the model
    # must be compared the same way
    want = [l.rstrip() for l in M.model(cp437=False).split('\n') if l.strip()]
    got, _ = screen('maze_ascii.png', frames=400)
    same = got[:len(want)] == want
    allok &= check('maze_ascii.png vs independent model',
                   same, f'{len(want)} lines' if same else repr(got[:1]))

    want_cp = [l for l in M.model(cp437=True).split('\n') if l.strip()]
    want_cp = [''.join(CP437[ord(c) & 255] for c in l) for l in want_cp]
    got, _ = screen('maze.png', frames=400)
    same = ([canon(l).rstrip() for l in got[:len(want_cp)]] ==
            [canon(l).rstrip() for l in want_cp])
    allok &= check('maze.png (CP437 default) vs model',
                   same, repr(got[0][:26]) if got else '')

    # Typing CP437: the program adds one to every byte it reads, so seeing
    # '▒▓B' back proves bytes B0/B1/41 arrived -- the screen echo alone would
    # prove only that the frontend drew what was typed.
    got, _ = screen('echo1.png', frames=300, typed=('\u2591\u2592A',))
    allok &= check('CP437 typed input reaches the program',
                   len(got) > 1 and got[1].startswith('\u2592\u2593B'),
                   repr(got[:2]))

    got, _ = screen('adv.png', frames=900, typed=('east',))
    joined = '\n'.join(got)
    allok &= check('adv.png reaches the first room',
                   'small room' in joined, repr(got[:1]))
    allok &= check('adv.png accepts a typed command',
                   'dark passage' in joined)

    got, _ = screen('lost_kingdom_pure_aheui.png', frames=1500, typed=('y',))
    joined = '\n'.join(got)
    allok &= check('lost kingdom banner', 'Lost Kingdom' in joined)
    allok &= check('lost kingdom copyright', 'Jon Ripley' in joined)
    allok &= check('lost kingdom accepts input', 'Ramshackle Hut' in joined)

    print('\n' + ('all core checks passed' if allok else 'FAILURES ABOVE'))
    return 0 if allok else 1


if __name__ == '__main__':
    sys.exit(main())
