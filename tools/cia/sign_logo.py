"""Launch-logo signing. HOME checks the logo before it draws it: the decompressed darc is followed by 32 bytes,
HMAC-SHA256(key, darc), and a logo whose files were edited but whose tail was kept makes HOME panic
(luma dumps from the `menu` process). The key is a Nintendo secret inside HOME's code; it is never stored in the repo.
It is read from $HOME/.3ds/logo_hmackey_text (64 hex characters; the same place yellows8's ctr-logobuilder uses).

    python -I tools/cia/sign_logo.py findkey <home_code.bin> [<known_logo.lz11 or cia>]   find the key in HOME's code
                                                                  (checks it against a logo whose tail is known good) and save it
    python -I tools/cia/sign_logo.py verify <logo.lz11 | cia>      does the tail match the darc? (needs the key)
    python -I tools/cia/sign_logo.py sign <logo.lz11 | cia> <out.bin>   re-sign: any edited logo -> 8 KB makerom -logo file
    python -I tools/cia/sign_logo.py tails <logo.lz11 | cia>...    no key needed: show each logo's darc size, tail and fileSize check
"""
import hashlib, hmac, os, re, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import logo

KEYFILE = os.path.join(os.path.expanduser('~'), '.3ds', 'logo_hmackey_text')


def load(path):
    """-> the LZ11 bytes of a logo given as a .lz11/.bin file or as a CIA (its ExeFS logo)."""
    b = open(path, 'rb').read()
    if path.lower().endswith('.cia'):
        z = logo.exefs_file(logo.ncch(path), b'logo')
        assert z, 'no logo in %s' % path
        return z
    return b


def split(raw):
    """decoded logo -> (darc, tail). The darc header's fileSize says where the darc ends."""
    fsz = struct.unpack_from('<I', raw, 0xC)[0]
    assert raw[:4] == b'darc' and fsz + 32 == len(raw), 'not darc + 32 byte tail (fileSize %d, decoded %d)' % (fsz, len(raw))
    return raw[:fsz], raw[fsz:]


def key():
    if not os.path.exists(KEYFILE):
        raise SystemExit('no key at %s: run findkey on HOME\'s code first' % KEYFILE)
    return bytes.fromhex(open(KEYFILE).read().strip())


def sign_darc(darc, k):
    """-> the 8 KB file makerom takes with -logo: LZ11(darc + HMAC), padded with zeros."""
    raw = darc + hmac.digest(k, darc, 'sha256')
    z = logo.lz11_enc(raw)
    assert logo.lz11_dec(z) == raw
    if len(z) > 0x2000:
        raise SystemExit('logo is %d bytes compressed: the file holds 8192' % len(z))
    return z + bytes(0x2000 - len(z))


def findkey(code_path, known):
    code = open(code_path, 'rb').read()
    darc, tail = split(logo.lz11_dec(known))
    print('searching %d bytes of HOME code for a key that gives %s over a %d byte darc' % (len(code), tail[:8].hex(), len(darc)))
    # plain 32 byte keys at any offset, then 64 hex characters in text form
    for off in range(0, len(code) - 31):
        if hmac.digest(code[off:off + 32], darc, 'sha256') == tail:
            print('found at offset 0x%x (binary)' % off)
            return code[off:off + 32]
    for m in re.finditer(rb'[0-9a-fA-F]{64}', code):
        try:
            k = bytes.fromhex(m.group().decode())
        except ValueError:
            continue
        if hmac.digest(k, darc, 'sha256') == tail:
            print('found at offset 0x%x (hex text)' % m.start())
            return k
    return None


def main():
    cmd = sys.argv[1]
    if cmd == 'tails':
        for p in sys.argv[2:]:
            raw = logo.lz11_dec(load(p))
            try:
                darc, tail = split(raw)
                print('%-40s darc %6d  tail %s' % (os.path.basename(p), len(darc), tail.hex()))
            except AssertionError as e:
                print('%-40s %s' % (os.path.basename(p), e))
    elif cmd == 'findkey':
        known = load(sys.argv[3]) if len(sys.argv) > 3 else None
        if known is None:
            raise SystemExit('give a logo with a known good tail (the stock Homebrew one, any CIA built without -logo)')
        k = findkey(sys.argv[2], known)
        if k is None:
            raise SystemExit('no key found in that file')
        os.makedirs(os.path.dirname(KEYFILE), exist_ok=True)
        open(KEYFILE, 'w').write(k.hex() + '\n')
        print('saved to', KEYFILE, '(sha256 of it: %s...)' % hashlib.sha256(k).hexdigest()[:12])
    elif cmd == 'verify':
        k = key()
        for p in sys.argv[2:]:
            darc, tail = split(logo.lz11_dec(load(p)))
            print('%-40s %s' % (os.path.basename(p), 'tail OK' if hmac.digest(k, darc, 'sha256') == tail else 'TAIL DOES NOT MATCH'))
    elif cmd == 'sign':
        darc, _old = split(logo.lz11_dec(load(sys.argv[2])))
        out = sign_darc(darc, key())
        open(sys.argv[3], 'wb').write(out)
        print('wrote', sys.argv[3], len(out), 'bytes')
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main()
