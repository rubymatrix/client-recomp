"""Find the compiled Direct3D 9 shaders (D3D bytecode) inside a file and write each out.

  python3 tools/extract_shaders.py <file> <out dir> [--min-version 2]

A shader starts with its version token (0xFFFE0000 | major << 8 | minor for a vertex shader,
0xFFFF... for a pixel shader) and ends at the END token (0x0000FFFF); in between, every
instruction token carries its length (bits 24-27, shader model 2 and up) and every comment its
own (bits 16-30), so a walk from the version token either lands on END exactly or the candidate is
not a shader. Written as <out>/<offset>.<vs|ps>_<major>_<minor>.bin. The output is Square Enix
code: keep it under build/ (gitignored).
"""
import argparse
import os
import struct

END = 0x0000FFFF


def walk(data, off):
    """The shader's length in bytes if a valid one starts at off, else 0."""
    n = (len(data) - off) // 4
    i = 1
    count = 0
    while i < n:
        tok = struct.unpack_from('<I', data, off + 4 * i)[0]
        if tok == END:
            return 4 * (i + 1) if count else 0
        op = tok & 0xFFFF
        if op == 0xFFFE:  # comment
            i += 1 + ((tok >> 16) & 0x7FFF)
            continue
        if tok & 0x80000000:
            return 0  # a parameter token where an instruction belongs
        if op > 96 and op not in (0xFFFD,):
            return 0
        length = (tok >> 24) & 0xF
        i += 1 + length
        count += 1
        if count > 20000:
            return 0
    return 0


def find(data, min_major=2):
    """Every valid shader in data: candidates come from a byte search for version tokens (any
    4-byte alignment relative to the file is checked, as packed data is not aligned)."""
    import re
    out = []
    pat = re.compile(rb'[\x00\x01\xff][\x02\x03][\xfe\xff]\xff')
    for m in pat.finditer(data):
        off = m.start()
        tok = struct.unpack_from('<I', data, off)[0]
        major, minor = (tok >> 8) & 0xFF, tok & 0xFF
        if not (min_major <= major <= 3) or minor not in (0, 1, 0xFF):
            continue
        size = walk(data, off)
        if size:
            out.append((off, 'vs' if (tok >> 16) == 0xFFFE else 'ps', major, minor, size))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file', help='a file, or a folder searched recursively')
    ap.add_argument('out')
    ap.add_argument('--min-version', type=int, default=2)
    a = ap.parse_args()
    import hashlib
    import mmap
    files = [a.file]
    if os.path.isdir(a.file):
        files = [os.path.join(d, f) for d, _, fs in os.walk(a.file) for f in fs]
    os.makedirs(a.out, exist_ok=True)
    seen, kinds = set(), {}
    for path in sorted(files):
        try:
            with open(path, 'rb') as fh:
                if os.path.getsize(path) < 64:
                    continue
                data = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
        except (OSError, ValueError):
            continue
        for off, kind, major, minor, size in find(data, a.min_version):
            blob = data[off:off + size]
            h = hashlib.sha1(blob).hexdigest()[:12]
            if h in seen:
                continue
            seen.add(h)
            ver = '%s_%d_%s' % (kind, major, minor if minor != 0xFF else 'x')
            with open(os.path.join(a.out, '%s.%s.bin' % (h, ver)), 'wb') as f:
                f.write(blob)
            kinds[ver] = kinds.get(ver, 0) + 1
        data.close()
    print('%d unique shaders: %s' % (len(seen), ', '.join('%s %d' % kv for kv in sorted(kinds.items()))))


if __name__ == '__main__':
    main()
