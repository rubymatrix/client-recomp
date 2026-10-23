"""The ffxivgame.exe launch argument, as ffxivboot (and Garlemald Client) builds it.

  python3 tools/xiv_args.py <56-char session id> [--tick N]

Prints `sqex0002<base64url(blowfish(" T =<tick> /LANG =en-us /REGION =2 /SERVER_UTC =1356916742
/SESSION_ID =<id>"))>!////`. The Blowfish key is the 8 hex digits of tick & ~0xFFFF, and the game
decrypts with its own GetTickCount(), so the host must start the guest's tick count at --tick
(xivboot: XIV_TICK) - it stays in the same 65536 ms window long enough for the game to read it.

The cipher is Blowfish on little-endian 32-bit halves (Rust's BlowfishLE, the game's own byte
order): each 4-byte word is swapped before and after standard (big-endian) Blowfish. Only whole
8-byte blocks are enciphered; the tail stays plain, as the original loop leaves it.
(Port of Garlemald Client src/crypto/command_line.rs.)
"""
import argparse
import base64
import struct

from Crypto.Cipher import Blowfish

DEFAULT_TICK = 0x00010000


def swap32(b):
    return b''.join(struct.pack('<I', struct.unpack('>I', b[i:i + 4])[0]) for i in range(0, len(b), 4))


V1_KEY = b'/a8h4mc9gf.x'  # sqex0001's fixed key (ffxivlogin, ffxivupdater accept it)
CHECK = 'fX1pGtdS5CAP4_VL'  # sqex0002's check character, by bits 16-19 of the tick
CHECK_T = (0x1f, 0x17, 0x35, 0x29, 0x06, 0x2d, 0x1d, 0x12, 0x39, 0x02, 0x00, 0x0f, 0x38, 0x3f, 0x15, 0x0b)
B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_'


def encode(fields, version=2, tick=DEFAULT_TICK):
    """Any key/value pairs as the launcher programs encode them (SqexArgs): ' T =<tick>' then
    ' /KEY =value' in byte order of the keys, spaces doubled; sqex0001 (fixed key, no check char)
    or sqex0002 (keyed to the tick window, with its check char)."""
    esc = lambda v: str(v).replace(' ', '  ')
    plain = ' T =%d' % tick + ''.join(' /%s =%s' % (esc(k), esc(fields[k])) for k in sorted(fields))
    buf = bytearray(plain.encode() + b'\0')
    key = V1_KEY if version == 1 else ('%08x' % (tick & ~0xFFFF & 0xFFFFFFFF)).encode()
    cipher = Blowfish.new(key, Blowfish.MODE_ECB)
    full = len(buf) & ~7
    buf[:full] = swap32(cipher.encrypt(swap32(bytes(buf[:full]))))
    enc = base64.b64encode(bytes(buf)).decode().replace('+', '-').replace('/', '_')
    check = '' if version == 1 else B64[CHECK_T[(tick >> 16) & 0xF]]
    return 'sqex000%d%s%s////' % (version, enc, check)


def decode(arg, tick=None):
    """The fields of a sqex0001/sqex0002 argument; sqex0002 needs the tick window it was made in
    (its ' T' field is inside the cipher)."""
    i = arg.index('sqex')
    version = int(arg[i + 4:i + 8])
    body = arg[i + 8:arg.index('////', i)]
    if version == 2:
        body = body[:-1]
        key = ('%08x' % (tick & ~0xFFFF & 0xFFFFFFFF)).encode()
    else:
        key = V1_KEY
    raw = bytearray(base64.b64decode(body.replace('-', '+').replace('_', '/') + '=' * (-len(body) % 4)))
    full = len(raw) & ~7
    raw[:full] = swap32(Blowfish.new(key, Blowfish.MODE_ECB).decrypt(swap32(bytes(raw[:full]))))
    plain = bytes(raw).split(b'\0')[0].decode(errors='replace')
    out, k = {}, None
    for part in plain.replace('  ', '\x01').split(' '):
        if part.startswith('/') or part == 'T':
            k = part.lstrip('/')
        elif part.startswith('=') and k is not None:
            out[k.replace('\x01', ' ')] = part[1:].replace('\x01', ' ')
    return out


def build(session_id, tick=DEFAULT_TICK):
    if len(session_id) != 56:
        raise SystemExit('the session id must be 56 characters')
    plain = (' T =%d /LANG =en-us /REGION =2 /SERVER_UTC =1356916742 /SESSION_ID =%s' % (tick, session_id)).encode()
    buf = bytearray(plain + b'\0')
    key = ('%08x' % (tick & ~0xFFFF & 0xFFFFFFFF)).encode()
    cipher = Blowfish.new(key, Blowfish.MODE_ECB)
    full = len(buf) & ~7
    buf[:full] = swap32(cipher.encrypt(swap32(bytes(buf[:full]))))
    enc = base64.b64encode(bytes(buf)).decode().replace('+', '-').replace('/', '_')
    return 'sqex0002%s!////' % enc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('session_id', nargs='?')
    ap.add_argument('--tick', type=lambda s: int(s, 0), default=DEFAULT_TICK)
    ap.add_argument('--field', action='append', default=[], metavar='KEY=VALUE',
                    help='encode these fields instead of the game argument')
    ap.add_argument('--v1', action='store_true', help='with --field: sqex0001, the fixed key')
    ap.add_argument('--decode', metavar='ARG', help='print the fields of a sqex argument')
    a = ap.parse_args()
    if a.decode:
        for k, v in decode(a.decode, a.tick).items():
            print('%s=%s' % (k, v))
    elif a.field:
        print(encode(dict(f.split('=', 1) for f in a.field), 1 if a.v1 else 2, a.tick))
    else:
        print(build(a.session_id, a.tick))


if __name__ == '__main__':
    main()
