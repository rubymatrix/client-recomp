"""FFXiMain recompiler driver.

  python recomp/recomp.py --meta <meta.json> --image <unpacked.dll> --out generated/ \
      [--functions 0x10317480,0x10312980,...] [--all] [--stats] [--hooks name=0x100863d6,...]
      [--wraps name=0x10080dc0,...]

--functions translates those entries plus everything they reach by direct call or tail jump
(the closure), which is what a differential test needs. --all translates every function in the
metadata. Output: funcs.h (prototypes), funcs_NNN.c (translations), table.c (address -> function
table for indirect calls), and with --stats a coverage report of unimplemented instructions.

--hooks names instructions where the host may step in (meta/builds.json "hooks"): before each, the
translation calls the host function pointer rt_hook_<name> (GuestFn, defined in table.c, NULL until
the host sets it) with the guest's registers stored, and reloads them after.

--wraps names whole functions the host may replace (meta/builds.json "wraps"): the translation of
f_XXXXXXXX becomes f_XXXXXXXX_body, and f_XXXXXXXX calls the host's rt_wrap_<name> (NULL until the
host sets it) instead of it when set. The host runs at the function's entry (esp at the return
address) and either calls rt_orig_<name> (the body) or returns for it (runtime.h rt_return).
Every call reaches the wrap: direct calls, tail jumps and rt_table (indirect calls, vtables).
"""
import argparse
import collections
import json
import os
import sys

import capstone
import pefile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from x86c import FunctionTranslator  # noqa: E402


class Program:
    def __init__(self, meta, image_path):
        self.meta = meta
        self.prefix = 'f_'  # translated function names: f_XXXXXXXX (FFXiMain), <module>_XXXXXXXX otherwise
        pe = pefile.PE(image_path, fast_load=True)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        if self.base != meta['image_base']:
            raise SystemExit('image base %#x does not match metadata %#x' % (self.base, meta['image_base']))
        self.image = pe.get_memory_mapped_image()
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.functions = {f['entry']: f['ranges'] for f in meta['functions']}
        self.entries = set(self.functions)
        self.switches = {s['at']: s['targets'] for s in meta['switches'] if s['targets']}
        self.referenced = set()
        self.hooks = {}  # address -> name (--hooks)
        self.hooked = set()
        self.wraps = {}  # function entry -> name (--wraps)
        self.relocs = self.text_relocations(pe)
        self.inner = self.add_data_entries(pe)
        self.inner |= self.add_branch_entries()

    def add_data_entries(self, pe):
        """Code the image's data points at (vtables, callbacks, exception handlers) is called
        indirectly, so each such address must be an entry. The metadata misses some: Ghidra folds a
        small function into the one that tail-jumps to it (FFXiMain 2026-09-03: 0x100542e0, slot 0
        of the vtable at 0x1032b69c, a range of 0x10054460), and leaves a few out altogether
        (0x1019d090). A folded one becomes an entry with its host's ranges (the translation starts
        at the entry, and every branch back into the host stays inside it); one outside every
        range, when it decodes as code up to a ret. Returns the entries that sit inside another
        function's body (the Windows loader must not put a 5-byte jmp there)."""
        import bisect
        import struct
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_BASERELOC']])
        text_lo, text_hi = self.meta['text']
        spans = sorted((lo, hi, entry) for entry, ranges in self.functions.items() for lo, hi in ranges)
        starts = [lo for lo, _, _ in spans]
        inner = set()
        for block in getattr(pe, 'DIRECTORY_ENTRY_BASERELOC', ()):
            for e in block.entries:
                if e.type != 3:  # IMAGE_REL_BASED_HIGHLOW
                    continue
                target = struct.unpack_from('<I', self.image, e.rva)[0]
                if not text_lo <= target < text_hi or target in self.functions:
                    continue
                i = bisect.bisect_right(starts, target) - 1
                if i >= 0 and spans[i][0] <= target < spans[i][1]:
                    if not self.on_boundary(spans[i][2], target):
                        continue  # inside one of the host's instructions: a table in .text, not code
                    self.functions[target] = [list(r) for r in self.functions[spans[i][2]]]
                    inner.add(target)
                else:
                    end = self.code_until_ret(target)
                    if end:
                        self.functions[target] = [[target, end]]
        self.add_code_pointer_entries(spans, starts)
        self.entries = set(self.functions)
        return inner

    def add_code_pointer_entries(self, spans, starts):
        """The same for code that takes a function's address as an immediate - a callback stored
        into a structure, an exception handler pushed - when the function lies outside every range
        (FFXiMain 2026-09-03: 0x100a30b4 stores 0x100a3130, called through [esi+0x54] when the
        settings menu opens). Those addresses are in .text's own relocations. Only immediates
        count: an address used as a displacement is a table read (switch tables, byte maps), and
        tables decode as code."""
        import bisect
        import struct
        text_lo, text_hi = self.meta['text']
        tables = {s['table'] for s in self.meta['switches']}
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        md.detail = True

        def span_of(a):
            i = bisect.bisect_right(starts, a) - 1
            return spans[i] if i >= 0 and spans[i][0] <= a < spans[i][1] else None

        decoded = {}
        for site in sorted(self.relocs):
            host = span_of(site)
            if not host:
                continue  # a table in a gap, not an instruction
            target = struct.unpack_from('<I', self.image, site - self.base)[0]
            if not text_lo <= target < text_hi or target in self.functions or target in tables or span_of(target):
                continue
            entry = host[2]
            if entry not in decoded:
                ins = sorted((i for lo, hi in self.functions[entry] for i in md.disasm(self.read(lo, hi - lo), lo)),
                             key=lambda i: i.address)
                decoded[entry] = ([i.address for i in ins], ins)
            addrs, ins = decoded[entry]
            k = bisect.bisect_right(addrs, site) - 1
            if k < 0:
                continue
            i = ins[k]
            if not i.imm_offset or i.address + i.imm_offset != site:
                continue  # a displacement: data
            end = self.code_until_ret(target)
            if end:
                self.functions[target] = [[target, end]]

    def add_branch_entries(self):
        """The same for direct branches: a call to an address that is not an entry, or a jump
        into another function's body (FFXiMain 2026-09-03: 0x101f320f calls 0x101f0260, a range
        of 0x1007ad70). Each target on one of the host's instruction boundaries becomes an entry,
        so the branch is a call or tail call into its own translation; one inside an instruction
        comes from data decoded as code (0x10069500 "jumps" into the operand of a call) and is left. Returns the
        entries inside another function's body."""
        import bisect
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)  # no detail: fast
        text_lo, text_hi = self.meta['text']
        spans = sorted((lo, hi, entry) for entry, ranges in self.functions.items() for lo, hi in ranges)
        starts = [lo for lo, _, _ in spans]

        def span_of(a):
            i = bisect.bisect_right(starts, a) - 1
            return spans[i] if i >= 0 and spans[i][0] <= a < spans[i][1] else None

        targets = {}
        for entry, ranges in list(self.functions.items()):
            own = [(lo, hi) for lo, hi in ranges]
            for lo, hi in ranges:
                for addr, size, mn, op in md.disasm_lite(self.read(lo, hi - lo), lo):
                    if mn != 'call' and not mn.startswith('j') and mn not in ('loop', 'loope', 'loopne'):
                        continue
                    if not op.startswith('0x'):
                        continue
                    t = int(op, 16)
                    if t in self.functions or not text_lo <= t < text_hi:
                        continue
                    if mn == 'call' or not any(a <= t < b for a, b in own):
                        targets.setdefault(t, mn == 'call')
        inner = set()
        for t, is_call in sorted(targets.items()):
            s = span_of(t)
            if s and s[2] != t and not self.on_boundary(s[2], t):
                continue  # inside one of the host's instructions: the branch is data decoded as code
            if s and s[2] != t:
                self.functions[t] = [list(r) for r in self.functions[s[2]]]
                inner.add(t)
            elif not s and is_call:
                end = self.code_until_ret(t)
                if end:
                    self.functions[t] = [[t, end]]
        self.entries = set(self.functions)
        return inner

    def on_boundary(self, host, a):
        """Whether a starts an instruction of host's ranges, decoded linearly."""
        if not hasattr(self, '_boundaries'):
            self._boundaries = {}
            self._md_lite = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        if host not in self._boundaries:
            self._boundaries[host] = {i[0] for lo, hi in self.functions[host]
                                      for i in self._md_lite.disasm_lite(self.read(lo, hi - lo), lo)}
        return a in self._boundaries[host]

    def code_until_ret(self, va, limit=0x1000):
        """The end of straight-line code from va to its first ret, or 0 if it does not look like
        code (it runs out, or reads ports: data tables inside .text decode as in/ins)."""
        for ins in self.md.disasm(self.read(va, limit), va):
            if ins.mnemonic in ('in', 'out', 'insb', 'insd', 'outsb', 'outsd', 'hlt', 'cli', 'sti'):
                return 0
            if ins.mnemonic.startswith('ret'):
                return ins.address + ins.size
        return 0

    def text_relocations(self, pe):
        """Every location in .text that holds an absolute image address.

        They are not in the PE relocation directory (which covers .rdata/.data only): the packer's
        stub applies them itself from a private table at the start of .reloc, in the ordinary
        base-relocation block format, ending at the first block past .text or of size 0.
        """
        import struct
        reloc = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.reloc')
        text = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.text')
        text_end = text.VirtualAddress + text.Misc_VirtualSize
        off = reloc.VirtualAddress
        out = set()
        while True:
            page, size = struct.unpack_from('<II', self.image, off)
            if size == 0 or page >= text_end:
                break
            for k in range((size - 8) // 2):
                e = struct.unpack_from('<H', self.image, off + 8 + 2 * k)[0]
                if e >> 12 == 3:  # IMAGE_REL_BASED_HIGHLOW
                    out.add(self.base + page + (e & 0xFFF))
            off += size
        return out

    def read(self, va, n):
        off = va - self.base
        return self.image[off:off + n]


def write_if_changed(path, text):
    """Leave unchanged files alone so an incremental C build only recompiles what moved."""
    try:
        with open(path) as f:
            if f.read() == text:
                return
    except OSError:
        pass
    with open(path, 'w') as f:
        f.write(text)


def patch_kinds(prog, entries):
    """0 if a 5-byte jmp fits at the entry without touching another entry or live bytes, else 1."""
    all_entries = sorted(prog.entries)
    nxt = {a: b for a, b in zip(all_entries, all_entries[1:])}
    kinds = []
    for e in entries:
        ok = e not in prog.inner and nxt.get(e, e + 5) - e >= 5
        if ok:
            end = max(hi for lo, hi in prog.functions[e] if lo <= e < hi) if any(lo <= e < hi for lo, hi in prog.functions[e]) else e
            if end < e + 5:
                tail = prog.read(end, e + 5 - end)
                ok = all(b in (0xCC, 0x90) for b in tail)
        kinds.append(0 if ok else 1)
    return kinds


def retail_dll():
    import winreg
    for view in (winreg.KEY_WOW64_32KEY, winreg.KEY_WOW64_64KEY):
        try:
            k = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'SOFTWARE\PlayOnlineUS\InstallFolder', 0, winreg.KEY_READ | view)
            return os.path.join(winreg.QueryValueEx(k, '0001')[0], 'FFXiMain.dll')
        except OSError:
            continue
    raise SystemExit('FINAL FANTASY XI install not found in the registry; pass --retail')


def image_constants(prog, image_path):
    """What the loader needs to rebuild .text from the retail DLL and check it is the right build."""
    sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools')))
    import unpack  # the static unpacker (tools/unpack.py)
    pe = pefile.PE(image_path, fast_load=True)
    text = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.text')
    packed = unpack.packed_section(pe)
    src_len, dst_len, oep = unpack.parse_stub(pe)
    reloc = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.reloc')
    return {
        'base': prog.base,
        'reloc_rva': reloc.VirtualAddress,
        'timestamp': pe.FILE_HEADER.TimeDateStamp,
        'size': pe.OPTIONAL_HEADER.SizeOfImage,
        'text_rva': text.VirtualAddress,
        'text_size': dst_len,
        'packed_rva': packed.VirtualAddress,
        'packed_len': src_len,
        'oep': prog.base + oep,
    }


def wrap(prog, e, lines):
    """--wraps: the translation renamed to <name>_body, and a new entry that defers to the host."""
    fn = '%s%08x' % (prog.prefix, e)
    head = 'void %s(Guest* g)' % fn
    if lines[0] != head:
        raise SystemExit('--wraps: unexpected translation header for %#x' % e)
    name = prog.wraps[e]
    return ['void %s_body(Guest* g)' % fn] + lines[1:] + [
        '',
        '/* recomp.py --wraps %s: the host may replace this function */' % name,
        head,
        '{',
        '    if (rt_wrap_%s) rt_wrap_%s(g);' % (name, name),
        '    else %s_body(g);' % fn,
        '}',
    ]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--meta', required=True)
    ap.add_argument('--image', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--functions', default='')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--stats', action='store_true')
    ap.add_argument('--chunk', type=int, default=400)
    ap.add_argument('--retail', help='the retail (packed) DLL; default: FFXiMain.dll from the registry')
    ap.add_argument('--hooks', default='', help='name=0xADDR,...: host hook points (see above)')
    ap.add_argument('--wraps', default='', help='name=0xADDR,...: functions the host may replace (see above)')
    ap.add_argument('--module', default='',
                    help='a second module (e.g. ffxi for FFXi.dll): functions are named <module>_XXXXXXXX, the '
                         'module has its own relocation delta, and table.c defines RtModule rt_module_<module> '
                         'instead of the rt_table/rt_image_* globals FFXiMain uses')
    args = ap.parse_args()

    meta = json.load(open(args.meta))
    prog = Program(meta, args.image)
    prog.prefix = args.module + '_' if args.module else 'f_'
    for h in filter(None, args.hooks.split(',')):
        name, addr = h.split('=')
        prog.hooks[int(addr, 16)] = name
    for w in filter(None, args.wraps.split(',')):
        name, addr = w.split('=')
        a = int(addr, 16)
        if a not in prog.entries:
            raise SystemExit('--wraps: %s=%#x is not a function entry in the metadata' % (name, a))
        prog.wraps[a] = name

    if args.all:
        todo = sorted(prog.entries)
        closure = False
    else:
        todo = [int(x, 16) for x in args.functions.split(',') if x]
        closure = True
        for e in todo:
            if e not in prog.entries:
                raise SystemExit('%#x is not a function entry in the metadata' % e)

    done = {}
    unimpl = collections.Counter()
    unimpl_funcs = set()
    queue = list(todo)
    while queue:
        e = queue.pop()
        if e in done:
            continue
        prog.referenced = set()
        t = FunctionTranslator(prog, e, prog.functions[e])
        done[e] = t.translate()
        if e in prog.wraps:
            done[e] = wrap(prog, e, done[e])
        for addr, mn, why in t.unimpl:
            unimpl[why if why.startswith('x87') else mn] += 1
            unimpl_funcs.add(e)
        if closure:
            queue.extend(r for r in prog.referenced if r not in done)

    os.makedirs(args.out, exist_ok=True)
    entries = sorted(done)
    # A second module relocates independently of FFXiMain: its translation reads its own delta.
    delta = ('\n#undef RD\n#define RD rt_delta_%s\nextern uint32_t rt_delta_%s;\n' % (args.module, args.module)
             if args.module else '')
    with open(os.path.join(args.out, 'funcs.h'), 'w') as f:
        f.write('/* generated by recomp.py - do not edit, do not commit */\n#pragma once\n#include "guest.h"\n%s\n' % delta)
        for e in entries:
            f.write('void %s%08x(Guest* g);\n' % (prog.prefix, e))
        for name in sorted(prog.hooks.values()):
            f.write('extern GuestFn rt_hook_%s;\n' % name)
        for e, name in sorted(prog.wraps.items()):
            f.write('void %s%08x_body(Guest* g);\nextern GuestFn rt_wrap_%s;\nextern const GuestFn rt_orig_%s;\n'
                    % (prog.prefix, e, name, name))
    chunks = set()
    for k in range(0, len(entries), args.chunk):
        name = 'funcs_%03d.c' % (k // args.chunk)
        chunks.add(name)
        text = ('/* generated by recomp.py - do not edit, do not commit */\n#include "funcs.h"\n\n'
                '#if defined(_MSC_VER)\n#pragma warning(disable: 4102 4189 4101 4702)\n'
                '#pragma code_seg(".xlat") /* translated code in its own section: the profiler tells it from the runtime */\n'
                '#endif\n\n')
        text += ''.join('\n'.join(done[e]) + '\n\n' for e in entries[k:k + args.chunk])
        write_if_changed(os.path.join(args.out, name), text)
    for old in os.listdir(args.out):
        if old.startswith('funcs_') and old.endswith('.c') and old not in chunks:
            os.remove(os.path.join(args.out, old))
    missed = sorted(set(prog.hooks) - prog.hooked)
    if missed and args.all:
        raise SystemExit('--hooks: %s is not an instruction in any translated function'
                         % ', '.join('%s=%#x' % (prog.hooks[a], a) for a in missed))
    kinds = patch_kinds(prog, entries)
    img = image_constants(prog, args.retail or retail_dll())
    with open(os.path.join(args.out, 'table.c'), 'w') as f:
        f.write('/* generated by recomp.py - do not edit, do not commit */\n#include "runtime.h"\n#include "funcs.h"\n\n')
        if args.module:
            # A second module: one descriptor the runtime registers (rt_add_module), and its delta.
            m = args.module
            f.write('uint32_t rt_delta_%s;\n\nstatic const RtEntry table[] = {\n' % m)
            for e in entries:
                f.write('    { 0x%08Xu, %s%08x },\n' % (e, prog.prefix, e))
            f.write('};\n\n/* The pinned retail build this translation belongs to. */\n')
            f.write('const RtModule rt_module_%s = {\n    "%s", table, %d, &rt_delta_%s,\n' % (m, m, len(entries), m))
            f.write('    ' + ', '.join('0x%Xu' % img[k] for k in ('base', 'timestamp', 'size', 'text_rva', 'text_size',
                                                                     'packed_rva', 'packed_len', 'oep', 'reloc_rva')))
            f.write(',\n};\n')
            print('module %s: translated %d functions (%d with unimplemented instructions)' % (m, len(entries), len(unimpl_funcs)))
            if args.stats or unimpl:
                for k, v in unimpl.most_common(40):
                    print('  %7d  %s' % (v, k))
            return
        for name in sorted(prog.hooks.values()):
            f.write('GuestFn rt_hook_%s; /* recomp.py --hooks */\n' % name)
        for e, name in sorted(prog.wraps.items()):
            f.write('GuestFn rt_wrap_%s; /* recomp.py --wraps */\nconst GuestFn rt_orig_%s = f_%08x_body;\n' % (name, name, e))
        if prog.hooks or prog.wraps:
            f.write('\n')
        f.write('const RtEntry rt_table[] = {\n')
        for e in entries:
            f.write('    { 0x%08Xu, f_%08x },\n' % (e, e))
        f.write('};\nconst unsigned rt_table_count = %d;\n\n' % len(entries))
        f.write('/* How each entry is redirected into its translation: 0 = 5-byte jmp, 1 = int3 (too close\n'
                ' * to the next entry, or a body shorter than 5 bytes with no padding after it). */\n')
        f.write('const unsigned char rt_table_patch[] = {\n')
        for k in range(0, len(kinds), 32):
            f.write('    ' + ','.join(str(x) for x in kinds[k:k + 32]) + ',\n')
        f.write('};\n\n')
        f.write('/* The pinned retail build this translation belongs to. */\n')
        for name, value in img.items():
            f.write('const uint32_t rt_image_%s = 0x%Xu;\n' % (name, value))
    print('entry patches: %d jmp, %d int3' % (kinds.count(0), kinds.count(1)))

    print('translated %d functions (%d with unimplemented instructions)' % (len(entries), len(unimpl_funcs)))
    if args.stats or unimpl:
        total = sum(unimpl.values())
        print('unimplemented instructions: %d' % total)
        for k, v in unimpl.most_common(40):
            print('  %7d  %s' % (v, k))


if __name__ == '__main__':
    main()
