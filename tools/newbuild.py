"""Bring up a retail build meta/builds.json does not know yet.

  python tools/newbuild.py identify --game <FINAL FANTASY XI folder>
        hashes, PE timestamps, the build label and client version, and whether each DLL is a
        known build or its .text is byte-identical to one (then its metadata carries over)
  python tools/newbuild.py unpack --game <folder>
        copy and unpack the retail DLLs into generated/images/<label>/, known build or not
  python tools/newbuild.py carry --from <old label> --to <new label> [--write] [--only modern]
        map every address the old build's entry names (builds.json addresses, hooks, modern and
        crt, the manual verdicts in discovery/verdicts.py) onto the new image; --write adds the new
        build to meta/builds.json and discovery/verdicts.py. Unmapped addresses are listed for a
        hand read. --only: just that section, written into a build builds.json already has.
  python tools/newbuild.py meta --from <old label> --to <new label> --module FFXi.dll
        a module whose .text is byte-identical to the old build's: its metadata, with the new hash
  python tools/newbuild.py dis --label <build> --at <hex> [--module FFXi.dll] [--before 32] [--count 16]
        disassemble an unpacked image around an address (decoding from up to --before bytes
        earlier, on a path that lands on the address), relocated operands marked: for checking a
        hand-mapped address or a discovery gate report

The images under generated/images/ are Square Enix code (gitignored, never committed); prepare.py
keeps one there for every build it unpacks, so the previous build is at hand for `carry`.

Mapping a code address: a byte window around it in the old .text, every dword the private .text
relocation table relocates wildcarded, must match exactly once in the new .text; windows widen
(both sides, then forward only, then backward only) until one does. A data address: every
relocated dword in the old .text that points at it (or near it) is a site; each site is mapped as
code and the new operand read there; the sites vote.
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys

import pefile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import buildinfo  # noqa: E402

ROOT = buildinfo.ROOT
IMAGES = os.path.join(ROOT, 'generated', 'images')
VERDICTS = os.path.join(ROOT, 'discovery', 'verdicts.py')
MODULES = ('FFXiMain.dll', 'FFXi.dll')
M64 = (1 << 64) - 1


# --- identify ------------------------------------------------------------------------------------
def pe_timestamp(path):
    return pefile.PE(path, fast_load=True).FILE_HEADER.TimeDateStamp


def label_of(ts):
    """New labels are the UTC date of FFXiMain.dll's PE timestamp (README, Rules)."""
    return datetime.datetime.fromtimestamp(ts, datetime.timezone.utc).strftime('%Y-%m-%d')


def inferred_version(ts):
    """An install with no patch.ver: '30' + the PE timestamp's YYMMDD, the pattern retail follows
    (2026-08-05 -> 30260805_0, the string its patch.ver carries)."""
    return '30%s_0' % datetime.datetime.fromtimestamp(ts, datetime.timezone.utc).strftime('%y%m%d')


def text_sha256(image):
    """SHA-256 of an unpacked image's .text (virtual size): equal means the code is identical."""
    pe = pefile.PE(image, fast_load=True)
    t = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.text')
    return hashlib.sha256(t.get_data()[:t.Misc_VirtualSize]).hexdigest()


def _tweak(ctr):
    t = ctr
    for _ in range(3):
        t = ((t << 10) | ctr) & M64
    return (t + 0xa1652347) & M64


def _byte_sum(k):
    return sum((k >> (8 * i)) & 0xff for i in range(8)) & 0xff


def _byte_stage(block, ctr, s):
    d = ((ctr >> 3) ^ 0x45) & 0xff
    x = bytearray(8)
    for k in range(8):
        x[k] = ((block[k] ^ d) + 0xc0 - s) & 0xff
        d = block[k]
    return int.from_bytes(x, 'little')


def _solve(v, t, k, bit, found):
    """Every K with ((v - K) ^ K) == t, bit by bit from the least significant."""
    if bit == 64:
        return found(k)
    mask = M64 if bit == 63 else (1 << (bit + 1)) - 1
    for b in (0, 1):
        kk = k | (b << bit)
        if ((((v - kk) & M64) ^ kk) & mask) == (t & mask) and _solve(v, t, kk, bit + 1, found):
            return True
    return False


def patch_ver_version(path):
    """The client version string in a retail patch.ver, or None. The core's file cipher with the
    key recovered from the file itself (the first two plaintext blocks are zero), as
    runtime/portable/gamecore_files.c does it; see specs/gamecore-slots.files.txt, slot 1171."""
    data = open(path, 'rb').read()
    if len(data) < 0x120:
        return None
    data = data[:0x120]
    hit = []
    for s0 in range(256):
        def found(k0):
            if _byte_sum(k0) != s0:
                return False
            k1 = (k0 * 5) & M64
            v1 = _byte_stage(data[8:16], 8, _byte_sum(k1))
            if ((((v1 - k1) & M64) ^ k1)) != _tweak(8):
                return False
            hit.append(k0)
            return True
        if _solve(_byte_stage(data[0:8], 0, s0), _tweak(0), 0, 0, found):
            break
    if not hit:
        return None
    keys = [hit[0]]
    for _ in range(31):
        keys.append((keys[-1] * 5) & M64)
    sums = [_byte_sum(k) for k in keys]
    out = bytearray()
    for off in range(0, 0x120, 8):
        idx = (off >> 3) & 31
        v = _byte_stage(data[off:off + 8], off, sums[idx])
        v = (v - keys[idx]) & M64
        v ^= _tweak(off)
        v ^= keys[idx]
        v = ((v >> 32) | (v << 32)) & M64
        out += v.to_bytes(8, 'little')
    return out[0x18:0x118].split(b'\0', 1)[0].decode('ascii', 'replace')


def identify(game):
    known = buildinfo.known()
    main = os.path.join(game, 'FFXiMain.dll')
    ts = pe_timestamp(main)
    info = {'game': game, 'label': label_of(ts), 'modules': {}}
    for m in MODULES:
        p = os.path.join(game, m)
        sha = buildinfo.sha256(p)
        match = next((lab for lab, b in known.items() if b[m]['sha256'] == sha), None)
        info['modules'][m] = {'sha256': sha, 'timestamp': '0x%08x' % pe_timestamp(p), 'known_build': match}
    info['known_build'] = info['modules']['FFXiMain.dll']['known_build']
    if info['known_build']:
        info['label'] = info['known_build']
    pv = os.path.join(game, 'patch.ver')
    version = patch_ver_version(pv) if os.path.exists(pv) else None
    info['version'] = version or inferred_version(ts)
    info['version_from'] = 'patch.ver' if version else 'PE timestamp date (no readable patch.ver)'
    return info


def identical_text(label, module):
    """Known builds whose unpacked .text for module equals label's (images in generated/images)."""
    mine = image(label, module)
    if not os.path.exists(mine):
        return []
    want = text_sha256(mine)
    out = []
    for other in buildinfo.known():
        p = image(other, module)
        if other != label and os.path.exists(p) and text_sha256(p) == want:
            out.append(other)
    return out


def image(label, module, kind='unpacked'):
    return os.path.join(IMAGES, label, '%s.%s.dll' % (module.split('.')[0], kind))


def unpack(game, label):
    d = os.path.join(IMAGES, label)
    os.makedirs(d, exist_ok=True)
    for m in MODULES:
        retail = image(label, m, 'retail')
        shutil.copyfile(os.path.join(game, m), retail)
        subprocess.check_call([sys.executable, os.path.join(HERE, 'unpack.py'), retail, image(label, m)])
    print('ok: %s' % d)


# --- carry -----------------------------------------------------------------------------------------
class Image:
    def __init__(self, path):
        pe = pefile.PE(path, fast_load=True)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        mapped = pe.get_memory_mapped_image()
        t = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.text')
        self.t0 = self.base + t.VirtualAddress
        self.text = mapped[t.VirtualAddress:t.VirtualAddress + t.Misc_VirtualSize]
        self.t1 = self.t0 + len(self.text)
        # The private .text relocation table the packer's stub applies (recomp/recomp.py, text_relocations).
        reloc = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.reloc')
        end = t.VirtualAddress + t.Misc_VirtualSize
        off = reloc.VirtualAddress
        self.relocs = set()
        while True:
            page, size = struct.unpack_from('<II', mapped, off)
            if size == 0 or page >= end:
                break
            for k in range((size - 8) // 2):
                e = struct.unpack_from('<H', mapped, off + 8 + 2 * k)[0]
                if e >> 12 == 3:
                    self.relocs.add(self.base + page + (e & 0xfff))
            off += size
        self.wild = bytearray(len(self.text))
        for r in self.relocs:
            i = r - self.t0
            if 0 <= i <= len(self.text) - 4:
                self.wild[i:i + 4] = b'\1\1\1\1'

    def dword(self, va):
        return struct.unpack_from('<I', self.text, va - self.t0)[0]

    def pattern(self, lo, hi):
        """A regex for old .text [lo, hi) with relocated dwords as wildcards."""
        parts, run = [], bytearray()
        for i in range(lo - self.t0, hi - self.t0):
            if self.wild[i]:
                if run:
                    parts.append(re.escape(bytes(run)))
                    run = bytearray()
                parts.append(b'.')
            else:
                run.append(self.text[i])
        if run:
            parts.append(re.escape(bytes(run)))
        return re.compile(b'(?=' + b''.join(parts) + b')', re.DOTALL)


WINDOWS = [(-n, n) for n in (16, 32, 64, 128, 256)] + [(0, n) for n in (24, 48, 96, 192, 384)] + \
          [(-n, 0) for n in (24, 48, 96, 192, 384)]


def map_code(old, new, va):
    """(new address, how) or (None, why)."""
    if not old.t0 <= va < old.t1:
        return None, 'not in .text'
    for before, after in WINDOWS:
        lo, hi = max(old.t0, va + before), min(old.t1, va + after if after else va + 1)
        if hi - lo < 12:
            continue
        hits = [m.start() for m in old.pattern(lo, hi).finditer(new.text)]
        if len(hits) == 1:
            return new.t0 + hits[0] + (va - lo), 'window %+d..%+d' % (lo - va, hi - va)
        if not hits:
            continue  # a change inside the window: try the others
    return None, 'no unique window'


def neighbour_hint(old, new, va):
    """For an address no window maps: how the code around it moved (a suggestion to verify by
    disassembling both images, not an answer)."""
    deltas = {}
    for d in (0x40, 0x80, 0x100, 0x200, 0x400):
        for a in (va - d, va + d):
            b, _ = map_code(old, new, a)
            if b is not None:
                deltas[b - a] = deltas.get(b - a, 0) + 1
    if not deltas:
        return 'neighbours unmapped too'
    best = max(deltas, key=deltas.get)
    return 'neighbours moved by %s -> try 0x%08x and check it by disassembly' % (
        ', '.join('%+#x (x%d)' % (k, n) for k, n in sorted(deltas.items(), key=lambda kv: -kv[1])), va + best)


def map_data(old, new, va, near=0x100, sites=None):
    """(new address, votes) for a data address, from the .text sites that reference it: those
    naming it exactly first, then those near it; at most `sites` of them mapped (each is a search of
    the whole new .text, and a global the game uses everywhere has hundreds)."""
    votes, mapped = {}, 0
    refs = [(abs(old.dword(r) - va), r) for r in sorted(old.relocs)
            if old.t0 <= r <= old.t1 - 4 and -near <= old.dword(r) - va <= near]
    for _, r in sorted(refs):
        if sites is not None and mapped >= sites:
            break
        k = old.dword(r) - va
        site, _ = map_code(old, new, r)
        if site is None:
            continue
        mapped += 1
        cand = new.dword(site) - k
        votes[cand] = votes.get(cand, 0) + (4 if k == 0 else 1)
    if not votes:
        return None, 'no referencing site mapped'
    best = max(votes, key=votes.get)
    return best, 'votes %d of %d' % (votes[best], sum(votes.values()))


def load_verdicts():
    ns = {}
    exec(compile(open(VERDICTS).read(), VERDICTS, 'exec'), ns)
    return ns


def carry(old_label, new_label, write, only=None):
    known = buildinfo.known()
    if old_label not in known:
        raise SystemExit('%s is not in meta/builds.json' % old_label)
    for lab in (old_label, new_label):
        for m in MODULES:
            if not os.path.exists(image(lab, m)):
                raise SystemExit('%s missing: tools/newbuild.py unpack --game <the %s install>' % (image(lab, m), lab))
    old_entry = known[old_label]
    ns = load_verdicts()
    report, failed = [], []

    def put(kind, name, a, b, how, old=None, new=None):
        if b is None and old is not None:
            how += '; ' + neighbour_hint(old, new, a)
        report.append('  %-10s %-14s 0x%08x -> %s  (%s)' % (kind, name, a, ('0x%08x' % b) if b else 'UNMAPPED', how))
        if b is None:
            failed.append((kind, name, a))
        return b

    main_old, main_new = Image(image(old_label, 'FFXiMain.dll')), Image(image(new_label, 'FFXiMain.dll'))
    entry = {'FFXiMain.dll': {}, 'FFXi.dll': {}, 'version': None, 'addresses': {}, 'hooks': {}, 'wraps': {}, 'modern': {}, 'crt': {}}
    if only:
        return carry_section(old_entry, new_label, only, main_old, main_new, put, report, failed, write)
    report.append('FFXiMain.dll addresses')
    for k, v in old_entry['addresses'].items():
        a = int(v, 16)
        # chars_ptr is a global (outside .text); the rest are code sites
        b, how = map_data(main_old, main_new, a) if not main_old.t0 <= a < main_old.t1 else map_code(main_old, main_new, a)
        put('address', k, a, b, how)
        entry['addresses'][k] = '0x%08x' % b if b else 'UNMAPPED from %s' % v
    for k, v in old_entry.get('hooks', {}).items():  # host hook points: code sites
        a = int(v, 16)
        b, how = map_code(main_old, main_new, a)
        put('hook', k, a, b, how)
        if b:
            entry['hooks'][k] = '0x%08x' % b
    for group, spots in old_entry.get('patches', {}).items():  # addons' code patches: code sites
        for addr, hexbytes in spots.items():
            a = int(addr, 16)
            b, how = map_code(main_old, main_new, a)
            put('patch', group, a, b, how)
            if b:
                entry.setdefault('patches', {}).setdefault(group, {})['0x%08x' % b] = hexbytes
    for k, v in old_entry.get('wraps', {}).items():  # host-replaceable functions: function entries
        a = int(v, 16)
        b, how = map_code(main_old, main_new, a)
        put('wrap', k, a, b, how)
        if b:
            entry['wraps'][k] = '0x%08x' % b
    report.append('FFXiMain.dll host/modern.c')
    entry['modern'] = map_modern(old_entry, main_old, main_new, put)
    report.append('FFXiMain.dll difftest CRT slice')
    for k, v in old_entry['crt'].items():
        a = int(v, 16)
        b, how = map_code(main_old, main_new, a)
        put('crt', k, a, b, how, main_old, main_new)
        entry['crt'][k] = '0x%08x' % b if b else 'UNMAPPED from %s' % v

    new_verdicts = {}
    for module in ('ffximain', 'ffxi'):
        key = (module, old_label)
        if key not in ns['VERDICTS']:
            report.append('no verdicts for %s %s' % key)
            continue
        dll = 'FFXiMain.dll' if module == 'ffximain' else 'FFXi.dll'
        o, n = Image(image(old_label, dll)), Image(image(new_label, dll))
        report.append('%s manual verdicts' % dll)
        v = ns['VERDICTS'][key]
        out = {'NOT_CODE': [], 'REDECODE': [], 'SWITCHES': {}}
        for kind in ('NOT_CODE', 'REDECODE'):
            for a in v[kind]:
                b, how = map_code(o, n, a)
                if put(kind, '', a, b, how, o, n):
                    out[kind].append(b)
        for a, cases in v['SWITCHES'].items():
            b, how = map_code(o, n, a)
            if put('SWITCH', '%d cases' % cases, a, b, how, o, n):
                out['SWITCHES'][b] = cases
        new_verdicts[module] = out

    print('\n'.join(report))
    if failed:
        print('\n%d unmapped: read them by hand (disassemble old and new around each; the same instruction '
              'usually moved by the same delta as its neighbours) and fix them in meta/builds.json / '
              'discovery/verdicts.py after --write' % len(failed))
    if not write:
        return
    ts = {m: pe_timestamp(image(new_label, m, 'retail')) for m in MODULES}
    for m in MODULES:
        entry[m] = {'sha256': buildinfo.sha256(image(new_label, m, 'retail')),
                    'meta': '%s.%s.meta.json' % (m.split('.')[0], new_label)}
    pv = os.path.join(IMAGES, new_label, 'version.txt')  # written by `unpack`, from identify
    version = open(pv).read().strip() if os.path.exists(pv) else None
    entry['version'] = version or inferred_version(ts['FFXiMain.dll'])
    with open(buildinfo.BUILDS) as f:
        doc = json.load(f)
    doc['builds'][new_label] = entry
    with open(buildinfo.BUILDS, 'w') as f:
        json.dump(doc, f, indent=2)
        f.write('\n')
    write_verdicts(new_label, ts, new_verdicts, old_label,
                   [line.strip() for line in report if 'UNMAPPED' in line and not line.lstrip().startswith(('address', 'hook', 'crt'))])
    print('\nwrote meta/builds.json (%s, version %s) and discovery/verdicts.py' % (new_label, entry['version']))


def map_modern(old_entry, main_old, main_new, put):
    """host/modern.c's addresses: functions and code (mapped as code), globals, vtables and menu-table
    entries (as data, from the code that names them). Unmapped ones stay as such: build.h then has
    FFXI_MODERN 0 until they are filled in by hand."""
    out = {}
    for k, v in old_entry.get('modern', {}).items():
        a = int(v, 16)
        if main_old.t0 <= a < main_old.t1:
            b, how = map_code(main_old, main_new, a)
        else:
            b, how = map_data(main_old, main_new, a, sites=16)
        put('modern', k, a, b, how)
        out[k] = '0x%08x' % b if b else 'UNMAPPED from %s' % v
    return out


SECTIONS = {'modern': map_modern}


def carry_section(old_entry, new_label, section, main_old, main_new, put, report, failed, write):
    """--only: one section carried into a build meta/builds.json already has, the rest of its entry
    (hand-checked addresses among it) left as it is."""
    report.append('FFXiMain.dll %s' % section)
    out = SECTIONS[section](old_entry, main_old, main_new, put)
    print('\n'.join(report))
    if failed:
        print('\n%d unmapped: read them by hand (newbuild.py dis in both builds) and fix them in '
              'meta/builds.json after --write' % len(failed))
    if not write:
        return
    with open(buildinfo.BUILDS) as f:
        doc = json.load(f)
    if new_label not in doc['builds']:
        raise SystemExit('%s is not in meta/builds.json: carry it without --only first' % new_label)
    doc['builds'][new_label][section] = out
    with open(buildinfo.BUILDS, 'w') as f:
        json.dump(doc, f, indent=2)
        f.write('\n')
    print('\nwrote meta/builds.json (%s: %s)' % (new_label, section))


def write_verdicts(label, ts, verdicts, old_label, unmapped):
    with open(VERDICTS, newline='') as f:
        src = f.read()

    def insert_before_close(src, opener, text):
        i = src.index(opener)
        j = src.index('\n}\n', i)
        return src[:j + 1] + text + src[j + 1:]

    ns = load_verdicts()
    lines = ''
    for m in MODULES:
        if ts[m] not in ns['BUILDS']:
            lines += "    0x%08x: '%s',  # %s\n" % (ts[m], label, m)
    if lines:
        src = insert_before_close(src, 'BUILDS = {', lines)
    body = '    # Carried over from %s by tools/newbuild.py carry; re-checked by the discovery pass.\n' % old_label
    for line in unmapped:
        body += '    # TODO unmapped, read by hand and add: %s\n' % line
    for module, v in verdicts.items():
        if (module, label) in ns['VERDICTS']:
            continue
        sw = ', '.join('0x%08x: %d' % (a, n) for a, n in sorted(v['SWITCHES'].items()))
        body += "    ('%s', '%s'): {\n" % (module, label)
        body += "        'NOT_CODE': [%s],\n" % ', '.join('0x%08x' % a for a in v['NOT_CODE'])
        body += "        'REDECODE': [%s],\n" % ', '.join('0x%08x' % a for a in v['REDECODE'])
        body += "        'SWITCHES': {%s},\n" % sw
        body += '    },\n'
    src = insert_before_close(src, 'VERDICTS = {', body)
    with open(VERDICTS, 'w', newline='') as f:
        f.write(src)


def carry_meta(old_label, new_label, module):
    if not identical_text(new_label, module) or old_label not in identical_text(new_label, module):
        raise SystemExit('%s .text differs between %s and %s: run the discovery pass (tools/discover.py)'
                         % (module, old_label, new_label))
    stem = module.split('.')[0]
    src = os.path.join(buildinfo.METADIR, '%s.%s.meta.json' % (stem, old_label))
    dst = os.path.join(buildinfo.METADIR, '%s.%s.meta.json' % (stem, new_label))
    meta = json.load(open(src))
    meta['sha256'] = buildinfo.sha256(image(new_label, module, 'retail'))
    meta['build'] = new_label
    with open(dst, 'w') as f:
        json.dump(meta, f, separators=(',', ':'))
    print('wrote %s (from %s, .text identical)' % (dst, src))


def disassemble(label, module, at, before, count):
    """Linear decode from `before` bytes earlier (x86 resynchronises within a few instructions),
    the instruction holding `at` marked '>'. When `at` is inside an instruction, not at its start,
    that is the evidence for a NOT_CODE verdict and the instruction's start is its REDECODE."""
    import capstone
    img = Image(image(label, module))
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    lo, end = max(img.t0, at - before), min(img.t1, at + 16 * count)
    shown = 0
    for a, size, mn, op in md.disasm_lite(img.text[lo - img.t0:end - img.t0], lo):
        if a > at and shown >= count:
            break
        raw = img.text[a - img.t0:a - img.t0 + size].hex()
        rel = any(r in img.relocs for r in range(a, a + size))
        mark = '>' if a <= at < a + size else ' '
        note = '   ; reloc' if rel else ''
        if mark == '>' and a != at:
            note += '   ; 0x%08x is INSIDE this instruction' % at
        print('%s%08x  %-20s %s %s%s' % (mark, a, raw, mn, op, note))
        if a + size > at:
            shown += 1


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('identify')
    p.add_argument('--game', required=True)
    p = sub.add_parser('unpack')
    p.add_argument('--game', required=True)
    p.add_argument('--label', help='default: the known build, else the PE timestamp date')
    p = sub.add_parser('carry')
    p.add_argument('--from', dest='old', required=True)
    p.add_argument('--to', dest='new', required=True)
    p.add_argument('--write', action='store_true')
    p.add_argument('--only', choices=sorted(SECTIONS), help='carry one section into a build already known')
    p = sub.add_parser('meta')
    p.add_argument('--from', dest='old', required=True)
    p.add_argument('--to', dest='new', required=True)
    p.add_argument('--module', required=True, choices=MODULES)
    p = sub.add_parser('dis')
    p.add_argument('--label', required=True)
    p.add_argument('--at', required=True, type=lambda x: int(x, 16))
    p.add_argument('--module', default='FFXiMain.dll', choices=MODULES)
    p.add_argument('--before', type=lambda x: int(x, 0), default=32)
    p.add_argument('--count', type=int, default=16)
    args = ap.parse_args()

    if args.cmd == 'identify':
        info = identify(os.path.normpath(args.game))
        for m in MODULES:
            same = identical_text(info['label'], m)
            info['modules'][m]['text_identical_to'] = same or None
        print(json.dumps(info, indent=2))
    elif args.cmd == 'unpack':
        info = identify(os.path.normpath(args.game))
        label = args.label or info['label']
        unpack(os.path.normpath(args.game), label)
        with open(os.path.join(IMAGES, label, 'version.txt'), 'w') as f:
            f.write(info['version'] + '\n')
        for m in MODULES:
            same = identical_text(label, m)
            print('%s: .text %s' % (m, ('identical to ' + ', '.join(same)) if same else 'differs from every unpacked known build'))
    elif args.cmd == 'carry':
        carry(args.old, args.new, args.write, args.only)
    elif args.cmd == 'meta':
        carry_meta(args.old, args.new, args.module)
    else:
        disassemble(args.label, args.module, args.at, args.before, args.count)


if __name__ == '__main__':
    main()
