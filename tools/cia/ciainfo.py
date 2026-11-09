"""What the HOME Menu reads out of one of our CIAs: the exheader's numbers, the ExeFS's files (with
hashes), the banner's parts. For comparing a CIA that launches with one that crashes the menu.

    uv run tools/cia/ciainfo.py player/builds/player.build38.cia player/builds/player.build53.cia
"""
import hashlib, struct, sys

def al(x, a=64): return (x + a - 1) // a * a

def parse(path):
    b = open(path, 'rb').read()
    hs, typ, ver, cert, tik, tmd, meta = struct.unpack_from('<IHHIIII', b, 0)
    csize = struct.unpack_from('<Q', b, 0x18)[0]
    off = al(hs) + al(cert) + al(tik) + al(tmd)
    n = b[off:off + csize]
    assert n[0x100:0x104] == b'NCCH', 'not an NCCH (encrypted?)'
    flags = n[0x188:0x190]
    out = {'file': path, 'ncch flags': flags.hex(), 'nocrypto': bool(flags[7] & 4)}
    exh = n[0x200:0x600]
    out['exheader title'] = exh[0:8].rstrip(b'\0').decode('ascii', 'replace')
    text = struct.unpack_from('<III', exh, 0x10)    # address, pages, size
    ro = struct.unpack_from('<III', exh, 0x20)
    data = struct.unpack_from('<III', exh, 0x30)
    stack = struct.unpack_from('<I', exh, 0x1C)[0]
    bss = struct.unpack_from('<I', exh, 0x3C)[0]
    out['text'] = '%08x %d pages %d' % text
    out['ro'] = '%08x %d pages %d' % ro
    out['data'] = '%08x %d pages %d' % data
    out['stack'] = stack
    out['bss'] = bss
    out['image pages (text+ro+data)'] = text[1] + ro[1] + data[1]
    out['image + bss bytes'] = (text[1] + ro[1] + data[1]) * 4096 + bss
    aci = exh[0x200:0x400]
    out['arm11 local caps (first 0x30)'] = aci[0:0x30].hex()
    # system mode etc: ARM11 local system capabilities flags at aci + 0x08 ... see 3dbrew
    out['aci flags'] = aci[0x08:0x10].hex()
    eo, es = struct.unpack_from('<II', n, 0x1A0)
    exefs = n[eo * 0x200:(eo + es) * 0x200]
    files = {}
    for k in range(10):
        name, fo, fs = struct.unpack_from('<8sII', exefs, k * 16)
        name = name.rstrip(b'\0').decode()
        if not name: continue
        data_ = exefs[0x200 + fo:0x200 + fo + fs]
        files[name] = (fs, hashlib.sha256(data_).hexdigest()[:12], data_)
    out['exefs'] = {k: (v[0], v[1]) for k, v in files.items()}
    if 'banner' in files:
        bn = files['banner'][2]
        out['banner magic'] = bn[0:4]
        cg = struct.unpack_from('<I', bn, 8)[0]
        wav = struct.unpack_from('<I', bn, 0x84)[0]
        out['banner cgfx@'] = hex(cg)
        out['banner cwav@'] = hex(wav)
        if wav and bn[wav:wav + 4] == b'CWAV':
            w = bn[wav:]
            out['cwav size'] = struct.unpack_from('<I', w, 0x0C)[0]
            # INFO block: encoding, loop, sample rate, loop start/end
            ib = struct.unpack_from('<I', w, 0x18 + 4)[0] if False else None
            io = struct.unpack_from('<I', w, 0x40 + 4)[0] if False else None
            # find INFO
            i = w.find(b'INFO')
            enc, loop = w[i + 8], w[i + 9]
            rate, ls, le = struct.unpack_from('<III', w, i + 0x0C)
            nch = struct.unpack_from('<I', w, i + 0x1C)[0]
            out['cwav'] = dict(encoding={0: 'PCM8', 1: 'PCM16', 2: 'DSP ADPCM', 3: 'IMA ADPCM'}.get(enc, enc), loop=loop, rate=rate,
                               samples=le, seconds=round(le / rate, 3) if rate else None, channels=nch)
    return out

if __name__ == '__main__':
    for p in sys.argv[1:]:
        o = parse(p)
        print('==', o.pop('file'))
        for k, v in o.items(): print('  %-30s %s' % (k, v))
