"""Report an XIV_SAMPLE profile: the functions the samples fell in, overall and per thread.

  python3 tools/xiv_sample.py <samples.bin> [--binary build/xivboot] [--top 40] [--skip-seconds 0]

Samples are (uint64 pc, uint64 lr, uint32 tid, pad) from runtime/portable/sampler_posix.c. PCs are
mapped to symbols with `nm -n` (the executable's load slide is found from main's address, which
xivboot logs as "[sample] main at 0x...").
"""
import argparse
import bisect
import collections
import re
import struct
import subprocess
import sys


def symbols(binary):
    out = subprocess.run(['nm', '-n', '--defined-only', binary], capture_output=True, text=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1].lower() == 't':
            addrs.append(int(parts[0], 16))
            names.append(parts[2].lstrip('_'))
    return addrs, names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('samples')
    ap.add_argument('--binary', default='build/xivboot')
    ap.add_argument('--slide', type=lambda s: int(s, 0), required=True, help='runtime address of main minus its nm address')
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--tid', type=int, help='only this thread')
    ap.add_argument('--after', type=float, default=0, help='only samples this many seconds after launch')
    a = ap.parse_args()
    addrs, names = symbols(a.binary)
    end = int(subprocess.run(['sh', '-c', "otool -l %s | awk '/sectname __text/{f=1} f&&/addr/{a=$2} f&&/size/{print a\" \"$2; exit}'" % a.binary],
                             capture_output=True, text=True).stdout.split()[0], 16)
    end += int(subprocess.run(['sh', '-c', "otool -l %s | awk '/sectname __text/{f=1} f&&/size/{print $2; exit}'" % a.binary],
                              capture_output=True, text=True).stdout.split()[0], 16)
    data = open(a.samples, 'rb').read()
    n = len(data) // 72
    by_fn = collections.Counter()
    by_tid = collections.Counter()
    by_tid_fn = collections.defaultdict(collections.Counter)
    for i in range(n):
        pc, lr, tid, ms, *fr = struct.unpack_from('<QQII6Q', data, 72 * i)
        if ms < a.after * 1000:
            continue
        if a.tid and tid != a.tid:
            continue
        k = bisect.bisect_right(addrs, pc - a.slide) - 1
        if pc - a.slide >= end:  # in a system library: named by the nearest caller in ours, [sys]
            fn = '[sys] ?'
            for r in [lr] + fr:
                if r and 0 < r - a.slide < end:
                    k = bisect.bisect_right(addrs, r - a.slide) - 1
                    fn = '[sys] ' + names[k]
                    break
        else:
            fn = names[k] if k >= 0 else '?'
        by_fn[fn] += 1
        by_tid[tid] += 1
        by_tid_fn[tid][fn] += 1
    total = sum(by_fn.values())
    print('%d samples' % total)
    print('\nthreads:')
    for tid, c in by_tid.most_common(10):
        top = ', '.join('%s %d%%' % (f, 100 * k // c) for f, k in by_tid_fn[tid].most_common(4))
        print('  %8d %5.1f%%  %s' % (tid, 100.0 * c / total, top))
    print('\nfunctions:')
    for fn, c in by_fn.most_common(a.top):
        print('  %5.1f%%  %s' % (100.0 * c / total, fn))


if __name__ == '__main__':
    main()
