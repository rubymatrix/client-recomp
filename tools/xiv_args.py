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
    ap.add_argument('session_id')
    ap.add_argument('--tick', type=lambda s: int(s, 0), default=DEFAULT_TICK)
    a = ap.parse_args()
    print(build(a.session_id, a.tick))


if __name__ == '__main__':
    main()
