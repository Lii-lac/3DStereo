"""ETC1 the 3DS way (bclim format 10): 8x8 tiles, each four 4x4 blocks (top-left, top-right,
bottom-left, bottom-right), every block a little-endian u64. decode() -> RGB rows, encode() <- RGB rows.
The encoder tries both modes (individual 444 / differential 555+333) and both flips per block, and
picks each pixel's modifier by least squared error: plenty for flat art.
"""
TABLES = [(2, 8), (5, 17), (9, 29), (13, 42), (18, 60), (24, 80), (33, 106), (47, 183)]
MODS = lambda t: (TABLES[t][0], TABLES[t][1], -TABLES[t][0], -TABLES[t][1])   # index by the 2 bits (msb, lsb) -> 0:+a 1:+b 2:-a 3:-b


def clamp(v): return 0 if v < 0 else 255 if v > 255 else v


def ext4(v): return v << 4 | v
def ext5(v): return v << 3 | v >> 2


def block_dec(w):
    """w: the block as a big-endian-order 64-bit int (ETC1's own order). -> 16 (r,g,b), index x*4+y."""
    hi, lo = w >> 32, w & 0xFFFFFFFF
    diff, flip = hi >> 1 & 1, hi & 1
    t1, t2 = hi >> 5 & 7, hi >> 2 & 7
    if diff:
        def d3(v): return v - 8 if v & 4 else v
        r, g, b = hi >> 27 & 31, hi >> 19 & 31, hi >> 11 & 31
        c1 = (ext5(r), ext5(g), ext5(b))
        c2 = (ext5((r + d3(hi >> 24 & 7)) & 31), ext5((g + d3(hi >> 16 & 7)) & 31), ext5((b + d3(hi >> 8 & 7)) & 31))
    else:
        c1 = (ext4(hi >> 28 & 15), ext4(hi >> 20 & 15), ext4(hi >> 12 & 15))
        c2 = (ext4(hi >> 24 & 15), ext4(hi >> 16 & 15), ext4(hi >> 8 & 15))
    out = [None] * 16
    for x in range(4):
        for y in range(4):
            i = x * 4 + y
            second = (y >= 2) if flip else (x >= 2)
            c, t = (c2, t2) if second else (c1, t1)
            m = MODS(t)[(lo >> (i + 16) & 1) << 1 | (lo >> i & 1)]
            out[i] = tuple(clamp(v + m) for v in c)
    return out


def _sub_pixels(px, flip, second):   # the 8 (index, rgb) of one half
    r = []
    for x in range(4):
        for y in range(4):
            if ((y >= 2) if flip else (x >= 2)) == second:
                r.append((x * 4 + y, px[x * 4 + y]))
    return r


def _best_table(base, pix):
    best = None
    for t in range(8):
        mods = MODS(t)
        err, sel = 0, []
        for i, p in pix:
            e, k = min((sum((clamp(c + m) - q) ** 2 for c, q in zip(base, p)), k) for k, m in enumerate(mods))
            err += e
            sel.append((i, k))
        if best is None or err < best[0]:
            best = (err, t, sel)
    return best


def block_enc(px):
    """px: 16 (r,g,b), index x*4+y -> the block as a 64-bit int (ETC1's order)."""
    best = None
    for flip in (0, 1):
        halves = [_sub_pixels(px, flip, s) for s in (False, True)]
        avgs = [tuple(sum(p[c] for _, p in h) / 8 for c in range(3)) for h in halves]
        # differential: 555 + a 3-bit signed delta, when the halves are close enough
        q5 = [tuple(min(31, max(0, round(a[c] * 31 / 255))) for c in range(3)) for a in avgs]
        dl = [q5[1][c] - q5[0][c] for c in range(3)]
        cands = []
        if all(-4 <= d <= 3 for d in dl):
            cands.append((1, q5[0], dl, [tuple(ext5(v) for v in q5[0]), tuple(ext5(v) for v in q5[1])]))
        q4 = [tuple(min(15, max(0, round(a[c] * 15 / 255))) for c in range(3)) for a in avgs]
        cands.append((0, q4, None, [tuple(ext4(v) for v in q) for q in q4]))
        for diff, q, dl_, bases in cands:
            r1 = _best_table(bases[0], halves[0])
            r2 = _best_table(bases[1], halves[1])
            err = r1[0] + r2[0]
            if best is None or err < best[0]:
                best = (err, diff, flip, q, dl_, r1, r2)
    err, diff, flip, q, dl, r1, r2 = best
    if diff:
        r, g, b = q
        hi = r << 27 | (dl[0] & 7) << 24 | g << 19 | (dl[1] & 7) << 16 | b << 11 | (dl[2] & 7) << 8
    else:
        (r1c, g1c, b1c), (r2c, g2c, b2c) = q
        hi = r1c << 28 | r2c << 24 | g1c << 20 | g2c << 16 | b1c << 12 | b2c << 8
    hi |= r1[1] << 5 | r2[1] << 2 | diff << 1 | flip
    lo = 0
    for i, k in r1[2] + r2[2]:
        lo |= (k >> 1 & 1) << (i + 16) | (k & 1) << i
    return hi << 32 | lo


def decode(data, w, h):
    """-> rows of (r,g,b), w x h (the texture's padded size)."""
    img = [[(0, 0, 0)] * w for _ in range(h)]
    pos = 0
    for ty in range(0, h, 8):
        for tx in range(0, w, 8):
            for by, bx in ((0, 0), (0, 4), (4, 0), (4, 4)):
                w64 = int.from_bytes(data[pos:pos + 8], 'little')
                pos += 8
                px = block_dec(w64)
                for x in range(4):
                    for y in range(4):
                        img[ty + by + y][tx + bx + x] = px[x * 4 + y]
    return img


def encode(img, w, h):
    out = bytearray()
    for ty in range(0, h, 8):
        for tx in range(0, w, 8):
            for by, bx in ((0, 0), (0, 4), (4, 0), (4, 4)):
                px = [img[ty + by + y][tx + bx + x] for x in range(4) for y in range(4)]
                out += block_enc(px).to_bytes(8, 'little')
    return bytes(out)
