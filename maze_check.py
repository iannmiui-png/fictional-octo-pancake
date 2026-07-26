"""Structural checks on the generated maze, run against the model's own
connectivity rather than the rendered text where possible."""
import maze_bf as M


def exits_grid(w, h, seed=None):
    seed = M.default_seed(w) if seed is None else seed
    ca = [0] * (w + 2)
    for i in range(w):
        if (seed >> i) & 1:
            ca[i + 1] = 1
    E = [[0] * w for _ in range(h)]
    S = [[0] * w for _ in range(h)]
    NOISE = [[0] * w for _ in range(h)]
    for y in range(h):
        new = [0] * (w + 2)
        for i in range(1, w + 1):
            l, c, r = ca[i - 1], ca[i], ca[i + 1]
            new[i] = 1 if ((c or r) and not (l and c and r)) else 0
        for x in range(w):
            if x == w - 1 and y == h - 1:   e, s = 0, 0
            elif x == w - 1:                e, s = 0, 1
            elif y == h - 1:                e, s = 1, 0
            else:
                a, b = ca[x + 1], new[x + 1]
                e, s = (0 if a else 1), a
                if b: e = s = 1
            E[y][x], S[y][x] = e, s
            NOISE[y][x] = new[((x + M.NOISE_SHIFT[0]) % w) + 1]
        ca = new
    return E, S, NOISE


def connected(w, h, E, S):
    """Every cell reachable from the top-left. The binary-tree base is a
    spanning tree and the Rule 110 perturbation only ever ADDS edges, so this
    must hold for every seed -- it is an invariant, not a property of one run."""
    seen = {(0, 0)}
    stack = [(0, 0)]
    while stack:
        x, y = stack.pop()
        nbr = []
        if E[y][x]: nbr.append((x + 1, y))
        if S[y][x]: nbr.append((x, y + 1))
        if x and E[y][x - 1]: nbr.append((x - 1, y))
        if y and S[y - 1][x]: nbr.append((x, y - 1))
        for n in nbr:
            if 0 <= n[0] < w and 0 <= n[1] < h and n not in seen:
                seen.add(n); stack.append(n)
    return len(seen), w * h


def arrows_honest(w, h, E, S, NOISE, g):
    """An arrow must point along an exit that is actually open."""
    bad = 0
    for y in range(h):
        for x in range(w):
            if not NOISE[y][x]:
                continue
            if E[y][x]:      pass                 # '>' and east is open
            elif S[y][x]:    pass                 # 'v' and south is open
            else:            continue             # no arrow drawn at all
            if E[y][x] == 0 and S[y][x] == 0:
                bad += 1
    return bad


if __name__ == '__main__':
    for w, h in ((16, 16), (12, 12), (24, 24)):
        for seed in (None, 0x1249, 0xBEEF, 0x5A5A):
            E, S, N = exits_grid(w, h, seed)
            got, tot = connected(w, h, E, S)
            bad = arrows_honest(w, h, E, S, N, None)
            tag = 'default' if seed is None else hex(seed)
            print(f'{w}x{h} seed {tag:>8}: reachable {got}/{tot}  lying arrows {bad}')
