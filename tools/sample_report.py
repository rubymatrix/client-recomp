"""Where the game thread's time goes: FFXI_SAMPLE's samples (runtime/portable/sampler_win.c) against
the host executable's linker map (/MAP).

  python tools/sample_report.py <samples.bin> <exe.map> [--top 40] [--skip-seconds 0]

Self time is the function a sample stopped in; inclusive time counts every function on the sample's
stack once. Recompiled game functions are f_<guest address>; everything else is the runtime, the
CRT, or (outside) a system DLL or the driver, reported with the first host frame that called it.
"""
import argparse
import bisect
import collections
import re
import struct
import sys

OUTSIDE = 0xFFFFFFFF


def load_map(path):
    base = None
    syms = []
    sym = re.compile(r'^\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\b')
    with open(path, errors='replace') as f:
        for line in f:
            if base is None:
                m = re.search(r'Preferred load address is ([0-9a-fA-F]+)', line)
                if m:
                    base = int(m.group(1), 16)
                continue
            m = sym.match(line)
            if m:
                va = int(m.group(2), 16)
                if va >= base:
                    syms.append((va - base, m.group(1)))
    if base is None:
        sys.exit('%s: not an MSVC map file' % path)
    syms.sort()
    return [s[0] for s in syms], [s[1] for s in syms]


def pretty(name):
    # C++ names come decorated: ?name@scope@@... -> scope::name
    if name.startswith('?') and '@' in name:
        parts = name[1:].split('@@')[0].split('@')
        return '::'.join(reversed([p for p in parts if p]))
    return name


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('samples')
    ap.add_argument('map')
    ap.add_argument('--top', type=int, default=40)
    args = ap.parse_args()

    addrs, names = load_map(args.map)
    cache = {}

    def name_of(rva):
        if rva == OUTSIDE:
            return '(outside the executable)'
        n = cache.get(rva)
        if n is None:
            i = bisect.bisect_right(addrs, rva) - 1
            n = cache[rva] = pretty(names[i]) if i >= 0 else '?'
        return n

    data = open(args.samples, 'rb').read()
    if data[:8] != b'FXSAMPL1':
        sys.exit('%s: not an FFXI_SAMPLE file' % args.samples)
    self_t = collections.Counter()
    incl = collections.Counter()
    outside_callers = collections.Counter()
    kinds = collections.Counter()
    total = 0
    o = 8
    while o + 4 <= len(data):
        (n,) = struct.unpack_from('<I', data, o)
        o += 4
        if o + 4 * n > len(data):
            break
        stack = struct.unpack_from('<%dI' % n, data, o)
        o += 4 * n
        total += 1
        frames = [name_of(r) for r in stack]
        top = frames[0]
        self_t[top] += 1
        if stack[0] == OUTSIDE:
            caller = next((f for r, f in zip(stack, frames) if r != OUTSIDE), '?')
            outside_callers[caller] += 1
            kinds['outside (system DLLs, driver)'] += 1
        elif re.match(r'f_[0-9a-f]{8}$', top):
            kinds['recompiled game code'] += 1
        else:
            kinds['runtime, CRT'] += 1
        for f in set(frames):
            incl[f] += 1
    if not total:
        sys.exit('no samples')

    def table(title, counter):
        print('\n%s' % title)
        for name, c in counter.most_common(args.top):
            print('  %6.2f%%  %7d  %s' % (100.0 * c / total, c, name))

    print('%d samples (about %.1f s of the game thread)' % (total, total / 1000.0))
    table('where the samples stopped, by kind', kinds)
    table('self: the function a sample stopped in', self_t)
    table('inclusive: on the stack', incl)
    if outside_callers:
        table('outside the executable: the host frame that called out', outside_callers)


if __name__ == '__main__':
    main()
