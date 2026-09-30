"""Collect the code patches addons make into meta/builds.json "patches" for the current build.

  python3 tools/addon_patches.py <survey log folder>... [--dry-run]

Addons (Ashita's instantchat, macrofix, XIUI...) write bytes over the game's code; recompiled code
never runs them unless the recompiler has translated a variant with them in place (recomp.py
--patches). The addon host logs every such write as

  [addons] codepatch <addon> at=<addr> fn=<function> old=<hex> new=<hex>

(host/addons/patch.c; "?" when a raw ffi store was found by comparison). Run the addons first, e.g.
`tools/addon_survey.py --kind ashita ... --keep-logs <folder>`, whose per-addon logs (<addon>.log)
also name the addon behind a "?" line. This replays each addon's writes in order, keeps each byte's
first patched value, and groups the bytes by addon and game function: one group is one translated
variant, chosen while all its bytes are in place. The groups replace the build's "patches".
"""
import argparse
import collections
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import buildinfo  # noqa: E402

LINE = re.compile(r'codepatch (\S+) at=([0-9a-f]+) fn=([0-9a-f]+) old=([0-9a-f.]*) new=([0-9a-f.]*)')


def ident(name):
    s = re.sub(r'[^A-Za-z0-9_]', '_', name)
    return s if s[:1].isalpha() else 'a_' + s


def collect(paths):
    """{(addon, function): {address: patched byte}}"""
    groups = collections.defaultdict(dict)
    for folder in paths:
        for f in sorted(os.listdir(folder)):
            if not f.endswith('.log'):
                continue
            addon = f[:-4]
            original = {}
            for line in open(os.path.join(folder, f), errors='replace'):
                m = LINE.search(line)
                if not m:
                    continue
                who, at, fn, old, new = m.groups()
                if '..' in old or '..' in new:
                    print('%s: a patch longer than the log shows at %s: skipped' % (addon, at))
                    continue
                who = addon if who == '?' else who
                a, fn = int(at, 16), int(fn, 16)
                ob, nb = bytes.fromhex(old), bytes.fromhex(new)
                for i, (o, n) in enumerate(zip(ob, nb)):
                    original.setdefault(a + i, o)
                    if n != original[a + i] and (a + i) not in groups[(who, fn)]:
                        groups[(who, fn)][a + i] = n
    return groups


def to_builds(groups):
    """builds.json form: {group: {"0xADDR": "hex", ...}} with runs of consecutive bytes merged."""
    out = {}
    names = collections.Counter(addon for addon, _ in groups)
    for (addon, fn), spots in sorted(groups.items()):
        if not spots:
            continue
        name = ident(addon if names[addon] == 1 else '%s_%08x' % (addon, fn))
        runs, cur, start, prev = {}, bytearray(), None, None
        for a in sorted(spots):
            if prev is not None and a == prev + 1:
                cur.append(spots[a])
            else:
                if cur:
                    runs['0x%08x' % start] = cur.hex()
                start, cur = a, bytearray([spots[a]])
            prev = a
        if cur:
            runs['0x%08x' % start] = cur.hex()
        out[name] = runs
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()
    build = buildinfo.current()['build']
    patches = to_builds(collect(a.logs))
    for name, runs in sorted(patches.items()):
        print('%-28s %s' % (name, ' '.join('%s:%s' % kv for kv in sorted(runs.items()))))
    if a.dry_run:
        return 0
    path = os.path.join(ROOT, 'meta', 'builds.json')
    data = json.load(open(path), object_pairs_hook=collections.OrderedDict)
    data['builds'][build]['patches'] = patches
    with open(path, 'w') as f:
        f.write(json.dumps(data, indent=2) + '\n')
    print('%d patch groups for %s in meta/builds.json' % (len(patches), build))
    return 0


if __name__ == '__main__':
    sys.exit(main())
