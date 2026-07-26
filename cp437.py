"""The CP437 code page, in the reading this project uses everywhere.

Python's own cp437 codec maps 0x00-0x1F to C0 control characters. The graphics
reading of that range -- where 0x18-0x1B are arrows and 0x01 is a smiley -- is
what the font, the libretro core and the browser console all use, so it lives
here once rather than being spelled out wherever it is needed.

Tab, newline and carriage return keep their control meaning; a terminal needs
them to lay text out at all.
"""

LOW = (' \u263A\u263B\u2665\u2666\u2663\u2660\u2022\u25D8\u25CB\u25D9\u2642'
       '\u2640\u266A\u266B\u263C\u25BA\u25C4\u2195\u203C\u00B6\u00A7\u25AC'
       '\u21A8\u2191\u2193\u2192\u2190\u221F\u2194\u25B2\u25BC')


def cp437_table():
    t = []
    for i in range(256):
        if i < 32:
            t.append(chr(i) if i in (9, 10, 13) else LOW[i])
        elif i < 127:
            t.append(chr(i))
        elif i == 127:
            t.append('\u2302')
        else:
            t.append(bytes([i]).decode('cp437'))
    return t
