"""Resolve a boundary trace written by the stand-in (runtime/win32/bridge.c).

  python tools/trace_report.py <FFXiMain.trace.txt>              summary: every host target the
                                                                  game called, grouped by module
  python tools/trace_report.py <FFXiMain.trace.txt> --seq <FFXiMain.seq.txt> [--grep TEXT] [--limit N]
                                                                  the calls in order, resolved

A target is named, in order of preference, as an import the loader resolved (DLL!name), an export
of the module it falls in, a gamecore common-function-table slot (gamecore!table+0xOFF, from the
unpacked gamecore), or module+RVA.
Call sites are static FFXiMain addresses; the call instruction there is disassembled, so a COM
call shows its vtable offset (`call dword ptr [ecx + 0x44]` is slot 17).
"""
import argparse
import collections
import os
import struct
import sys

import capstone
import pefile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# unpacked retail DLLs (gamecore for slot names): local only, never committed
UNPACKED = next((d for d in (os.path.join(ROOT, 'generated'),
                             os.path.normpath(os.path.join(ROOT, '..', 'ffxi-re', 'unpacked')))
                 if os.path.exists(os.path.join(d, 'gamecore.unpacked.dll'))), os.path.join(ROOT, 'generated'))
GAMECORE_TABLE = (0x1006fbe8, 0x10071448)  # static VA range in the unpacked gamecore


class Trace:
    def __init__(self, path):
        self.modules = []   # (base, size, path)
        self.imports = {}   # host address -> DLL!name
        self.calls = []     # (target, site, count)
        self.entries = []   # (guest fn, host return, count)
        self.header = ''
        with open(path) as f:
            for line in f:
                p = line.split()
                if not p:
                    continue
                if p[0] == '#':
                    self.header = line.strip()
                elif p[0] == 'module':
                    self.modules.append((int(p[1], 16), int(p[2], 16), ' '.join(p[3:])))
                elif p[0] == 'import':
                    self.imports.setdefault(int(p[1], 16), p[2])
                elif p[0] == 'call':
                    self.calls.append((int(p[1], 16), int(p[2], 16), int(p[3])))
                elif p[0] == 'entry':
                    self.entries.append((int(p[1], 16), int(p[2], 16), int(p[3])))

    def module_of(self, addr):
        for base, size, path in self.modules:
            if base <= addr < base + size:
                return base, path
        return None, None


class Resolver:
    def __init__(self, trace):
        self.t = trace
        self.exports = {}
        self.cache = {}
        self.gamecore_slots = self.load_gamecore_table()
        self.game = pefile.PE(os.path.join(UNPACKED, 'FFXiMain.unpacked.dll'), fast_load=True)
        self.game_img = self.game.get_memory_mapped_image()
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    @staticmethod
    def load_gamecore_table():
        path = os.path.join(UNPACKED, 'gamecore.unpacked.dll')
        if not os.path.exists(path):
            return {}
        pe = pefile.PE(path, fast_load=True)
        img = pe.get_memory_mapped_image()
        base = pe.OPTIONAL_HEADER.ImageBase
        slots = collections.defaultdict(list)  # function RVA -> [table offsets]
        lo, hi = GAMECORE_TABLE
        for va in range(lo, hi, 4):
            v = struct.unpack_from('<I', img, va - base)[0]
            if v:
                slots[v - base].append(va - lo)
        return slots

    def module_exports(self, path):
        if path not in self.exports:
            names = {}
            try:
                pe = pefile.PE(path, fast_load=True)
                pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT']])
                if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
                    for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
                        names[e.address] = e.name.decode() if e.name else '#%d' % e.ordinal
            except (OSError, pefile.PEFormatError):
                pass
            self.exports[path] = names
        return self.exports[path]

    def target(self, addr):
        if addr in self.cache:
            return self.cache[addr]
        name = self.t.imports.get(addr)
        if not name:
            base, path = self.t.module_of(addr)
            if base is None:
                name = '?%08x' % addr
            else:
                mod = os.path.basename(path)
                rva = addr - base
                exp = self.module_exports(path).get(rva)
                if exp:
                    name = '%s!%s' % (mod, exp)
                elif mod.lower() == 'gamecore' and rva in self.gamecore_slots:
                    name = 'gamecore!table+%s' % '/'.join('0x%x' % o for o in self.gamecore_slots[rva])
                else:
                    name = '%s+%x' % (mod, rva)
        self.cache[addr] = name
        return name

    def call_insn(self, site):
        """The call instruction that returns to `site` (a static FFXiMain address)."""
        off = site - self.game.OPTIONAL_HEADER.ImageBase
        for n in (2, 3, 6, 7, 5, 4):
            if off - n < 0:
                continue
            code = bytes(self.game_img[off - n:off])
            ins = list(self.md.disasm(code, site - n, count=1))
            if ins and ins[0].size == n and ins[0].mnemonic == 'call':
                return '%s %s' % (ins[0].mnemonic, ins[0].op_str)
        return '?'


def group_of(name):
    if name.startswith('gamecore!table'):
        return 'gamecore table'
    return name.split('!')[0].split('+')[0].lower()


def summary(trace, res):
    print(trace.header)
    per_target = collections.defaultdict(lambda: [0, set()])
    for target, site, count in trace.calls:
        name = res.target(target)
        per_target[name][0] += count
        per_target[name][1].add(site)
    groups = collections.defaultdict(list)
    for name, (count, sites) in per_target.items():
        groups[group_of(name)].append((count, name, sites))
    for g in sorted(groups, key=lambda k: -sum(c for c, _, _ in groups[k])):
        items = sorted(groups[g], reverse=True)
        print('\n== %s: %d targets, %d calls' % (g, len(items), sum(c for c, _, _ in items)))
        for count, name, sites in items:
            shown = sorted(sites)[:4]
            where = ', '.join('%08x %s' % (s, res.call_insn(s)) for s in shown)
            more = ' (+%d sites)' % (len(sites) - 4) if len(sites) > 4 else ''
            print('  %10d  %-48s %s%s' % (count, name, where, more))
    print('\n== host -> guest entries: %d distinct' % len(trace.entries))
    for fn, ret, count in sorted(trace.entries, key=lambda e: -e[2])[:200]:
        base, path = trace.module_of(ret)
        who = '%s+%x' % (os.path.basename(path), ret - base) if base is not None else '%08x' % ret
        print('  %10d  f_%08x  from %s' % (count, fn, who))


def sequence(trace, res, seq_path, grep, limit):
    shown = 0
    rets = {}
    lines = []
    with open(seq_path) as f:
        for line in f:
            p = line.split()
            if p and p[0] == 'r':
                rets[p[1]] = p[2]
            elif p and p[0] in ('c', 'e'):
                lines.append(p + [''])
            elif p and p[0] == 's' and lines:
                lines[-1][-1] += '  arg%s=%s' % (p[1], line.split(' ', 2)[2].strip())
    for p in lines:
        if p[0] == 'c':
            text = '%7s %8s t%-6s %-44s at %s  (%s)  = %s%s' % (p[1], p[2], p[3], res.target(int(p[4], 16)), p[5],
                                                                  ' '.join(p[6:10]), rets.get(p[1], '?'), p[-1])
        else:
            text = '%7s %8s t%-6s -> f_%s  (host ret %s)' % (p[1], p[2], p[3], p[4], p[5])
        if grep and grep.lower() not in text.lower():
            continue
        print(text)
        shown += 1
        if limit and shown >= limit:
            break


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('trace')
    ap.add_argument('--seq')
    ap.add_argument('--grep')
    ap.add_argument('--limit', type=int, default=0)
    a = ap.parse_args()
    trace = Trace(a.trace)
    res = Resolver(trace)
    if a.seq:
        sequence(trace, res, a.seq, a.grep, a.limit)
    else:
        summary(trace, res)


if __name__ == '__main__':
    sys.exit(main())
