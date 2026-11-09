"""The launch logo (the splash after HOME starts an app): pull it out of a CIA, unpack its darc, and
pack a darc back into the LZ11 file makerom takes with -logo.

    python -I tools/cia/logo.py unpack <cia> <dir>      dir/logo.lz, dir/logo.darc, dir/files/...
    python -I tools/cia/logo.py pack <dir> <out.bin>    dir/files/... (same names, same order) -> out.bin
"""
import os, struct, sys


def al(x, a=64): return (x + a - 1) // a * a


def ncch(path):
    b = open(path, 'rb').read()
    hs, typ, ver, cert, tik, tmd, meta = struct.unpack_from('<IHHIIII', b, 0)
    return b[al(hs) + al(cert) + al(tik) + al(tmd):]


def exefs_file(n, want):
    eo, es = struct.unpack_from('<II', n, 0x1A0)
    exefs = n[eo * 0x200:(eo + es) * 0x200]
    for k in range(10):
        nm, fo, fs = struct.unpack_from('<8sII', exefs, k * 16)
        if nm.rstrip(b'\x00') == want:
            return exefs[0x200 + fo:0x200 + fo + fs]
    lo, ls = struct.unpack_from('<II', n, 0x198)   # not in the ExeFS: the NCCH's own logo region
    return n[lo * 0x200:(lo + ls) * 0x200] if ls else None


def lz11_dec(d):
    assert d[0] == 0x11, 'not LZ11'
    n = d[1] | d[2] << 8 | d[3] << 16
    i = 4
    if n == 0:
        n = struct.unpack_from('<I', d, 4)[0]
        i = 8
    out = bytearray()
    while len(out) < n:
        flags = d[i]
        i += 1
        for bit in range(8):
            if len(out) >= n:
                break
            if flags & (0x80 >> bit):
                b0 = d[i]
                ind = b0 >> 4
                if ind == 0:
                    ln = ((b0 & 0xF) << 4 | d[i + 1] >> 4) + 0x11
                    disp = ((d[i + 1] & 0xF) << 8 | d[i + 2]) + 1
                    i += 3
                elif ind == 1:
                    ln = ((b0 & 0xF) << 12 | d[i + 1] << 4 | d[i + 2] >> 4) + 0x111
                    disp = ((d[i + 2] & 0xF) << 8 | d[i + 3]) + 1
                    i += 4
                else:
                    ln = ind + 1
                    disp = ((b0 & 0xF) << 8 | d[i + 1]) + 1
                    i += 2
                for _ in range(ln):
                    out.append(out[-disp])
            else:
                out.append(d[i])
                i += 1
    return bytes(out)


def lz11_enc(d, maxlen=0x10110, overlap=True):
    """Greedy LZ11 with a hash of 3-byte prefixes: small enough for an 8 KB logo. maxlen caps a copy; overlap=False
    only copies bytes already finished (disp >= len): the plainest stream a decoder can be handed."""
    n = len(d)
    out = bytearray([0x11, n & 0xFF, n >> 8 & 0xFF, n >> 16 & 0xFF])
    heads = {}
    i = 0
    while i < n:
        fpos = len(out)
        out.append(0)
        flags = 0
        for bit in range(8):
            if i >= n:
                break
            best_len, best_disp = 0, 0
            key = d[i:i + 3]
            for j in reversed(heads.get(key, [])[-64:]):
                disp = i - j
                if disp > 0x1000:
                    break
                if disp < 2:   # Nintendo's own stream never copies from the byte just before (its smallest is 2: the "VRAM-safe" form), so neither do we
                    continue
                ln = 0
                lim = maxlen if overlap else min(maxlen, disp)
                while ln < lim and i + ln < n and d[j + ln] == d[i + ln]:
                    ln += 1
                if ln > best_len:
                    best_len, best_disp = ln, disp
            if best_len >= 3:
                flags |= 0x80 >> bit
                dd = best_disp - 1
                if best_len <= 0x10:
                    out += bytes([(best_len - 1) << 4 | dd >> 8, dd & 0xFF])
                elif best_len <= 0x110:
                    l = best_len - 0x11
                    out += bytes([l >> 4, (l & 0xF) << 4 | dd >> 8, dd & 0xFF])
                else:
                    l = best_len - 0x111
                    out += bytes([0x10 | l >> 12, l >> 4 & 0xFF, (l & 0xF) << 4 | dd >> 8, dd & 0xFF])
                step = best_len
            else:
                out.append(d[i])
                step = 1
            for k in range(step):
                heads.setdefault(d[i + k:i + k + 3], []).append(i + k)
            i += step
        out[fpos] = flags
    while len(out) % 4:
        out.append(0)
    return bytes(out)


def darc_files(raw):
    magic, bom, hsz, ver, fsz, toff, tsz, doff = struct.unpack_from('<4sHHIIIII', raw, 0)
    assert magic == b'darc', magic
    count = struct.unpack_from('<III', raw, toff)[2]
    names = toff + 12 * count
    out = []
    for k in range(count):
        no, off, sz = struct.unpack_from('<III', raw, toff + 12 * k)
        s = raw[names + (no & 0xFFFFFF):]
        e = 0
        while s[e:e + 2] != b'\x00\x00':
            e += 2
        out.append((k, bool(no >> 24 & 1), s[:e].decode('utf-16le'), off, sz))
    return out


if __name__ == '__main__':
    if sys.argv[1] == 'unpack':
        cia, d = sys.argv[2], sys.argv[3]
        lz = exefs_file(ncch(cia), b'logo')
        raw = lz11_dec(lz)
        os.makedirs(os.path.join(d, 'files'), exist_ok=True)
        open(os.path.join(d, 'logo.lz'), 'wb').write(lz)
        open(os.path.join(d, 'logo.darc'), 'wb').write(raw)
        print('logo', len(lz), 'bytes, darc', len(raw))
        for k, isdir, nm, off, sz in darc_files(raw):
            if isdir:
                print('  dir ', k, repr(nm), 'parent', off, 'next', sz)
            else:
                print('  file', k, nm, 'at', off, sz, 'bytes', raw[off:off + 4])
                open(os.path.join(d, 'files', nm), 'wb').write(raw[off:off + sz])
    elif sys.argv[1] == 'pack':   # same darc, each file's bytes swapped for files/<name> (same size)
        d, outp = sys.argv[2], sys.argv[3]
        raw = bytearray(open(os.path.join(d, 'logo.darc'), 'rb').read())
        for k, isdir, nm, off, sz in darc_files(bytes(raw)):
            if isdir:
                continue
            new = open(os.path.join(d, 'files', nm), 'rb').read()
            assert len(new) == sz, (nm, len(new), sz)
            raw[off:off + sz] = new
        lz = lz11_enc(bytes(raw))
        assert lz11_dec(lz) == bytes(raw)
        if len(lz) < 0x2000:   # every built-in logo is exactly 8 KB: ours too, zeros after (LZ11 stops at its size)
            lz += bytes(0x2000 - len(lz))
        open(outp, 'wb').write(lz)
        print(outp, len(lz), 'bytes (LZ11), darc', len(raw))
