"""Reference Brainfuck interpreter: 8-bit wrapping cells, EOF = 0."""
import sys

def run(code, inp='', limit=200_000_000):
    code = [c for c in code if c in '+-.,<>[]']
    jm, st = {}, []
    for i, c in enumerate(code):
        if c == '[': st.append(i)
        elif c == ']': j = st.pop(); jm[j] = i; jm[i] = j
    t = bytearray(70000); p = ip = n = 0; out = []; inp = list(inp)
    while ip < len(code):
        c = code[ip]
        if   c == '+': t[p] = (t[p] + 1) & 255
        elif c == '-': t[p] = (t[p] - 1) & 255
        elif c == '>': p += 1
        elif c == '<': p -= 1
        elif c == '.': out.append(chr(t[p]))
        elif c == ',': t[p] = ord(inp.pop(0)) if inp else 0
        elif c == '[' and not t[p]: ip = jm[ip]
        elif c == ']' and t[p]:     ip = jm[ip]
        ip += 1; n += 1
        if n > limit: raise RuntimeError('step limit')
    return ''.join(out), n

if __name__ == '__main__':
    o, n = run(open(sys.argv[1]).read())
    sys.stderr.write(f'{n:,} steps\n')
    sys.stdout.write(o)
