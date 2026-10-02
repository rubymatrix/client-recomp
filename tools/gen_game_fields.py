"""Generate host/addons/game_fields.inc: field descriptors for the game structures, from Ashita's SDK.

  python3 tools/gen_game_fields.py [path/to/Ashita-v4beta/plugins/sdk/ffxi]

Reads the SDK's ffxi/*.h (entity.h, party.h, player.h, target.h, inventory.h, autofollow.h,
castbar.h; enums.h for the array bounds), lays each struct out the way MSVC does for 32-bit x86
(natural alignment, uintptr_t = 4 bytes, bitfields packed into units of their declared type),
checks every size against the header's own static_assert, and writes one descriptor table
(name -> offset, type, count, stride, nested struct) that host/addons/game.c walks. One generic
getter/setter path instead of a thousand hand-written ones.

The default SDK path is ../addon-deps/Ashita-v4beta/plugins/sdk/ffxi next to the repository.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_SDK = os.path.join(os.path.dirname(ROOT), 'addon-deps', 'Ashita-v4beta', 'plugins', 'sdk', 'ffxi')
OUT = os.path.join(ROOT, 'host', 'addons', 'game_fields.inc')

HEADERS = ('entity.h', 'autofollow.h', 'castbar.h', 'party.h', 'player.h', 'target.h', 'inventory.h')

# C type -> (descriptor type, size)
PRIM = {
    'uint8_t': ('U8', 1), 'int8_t': ('I8', 1), 'char': ('I8', 1), 'bool': ('U8', 1),
    'uint16_t': ('U16', 2), 'int16_t': ('I16', 2),
    'uint32_t': ('U32', 4), 'int32_t': ('I32', 4), 'uintptr_t': ('PTR', 4),
    'uint64_t': ('U64', 8), 'int64_t': ('U64', 8), 'float': ('F32', 4),
}


def enum_values(text):
    """Enums::X::Max for every enum class (only the implicit-counting ones we need)."""
    out = {}
    for m in re.finditer(r'enum\s+class\s+(\w+)\s*:\s*\w+\s*\{(.*?)\};', text, re.S):
        name, body = m.group(1), m.group(2)
        body = re.sub(r'//[^\n]*', '', body)
        v = -1
        for item in [s.strip() for s in body.split(',') if s.strip()]:
            if '=' in item:
                k, e = [s.strip() for s in item.split('=', 1)]
                try:
                    v = int(e, 0)
                except ValueError:
                    v = v + 1  # expression we don't need
            else:
                k = item
                v += 1
            out['Enums::%s::%s' % (name, k)] = v
    return out


def strip(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    text = re.sub(r'//[^\n]*', '', text)
    return text


def tokenize(text):
    return re.findall(r'[A-Za-z_]\w*(?:::\w+)*|0x[0-9A-Fa-f]+|\d+|[{}\[\];:,()=+\-*/<>&|.!?~]', text)


class Parser:
    def __init__(self, enums):
        self.enums = enums
        self.structs = {}  # name -> dict(kind, fields=[...]) ; field = (name, ctype, count, bits)
        self.order = []

    def eval_expr(self, toks):
        s = ' '.join(toks)
        s = s.replace('( uint32_t )', '').replace('(uint32_t)', '')
        for k, v in self.enums.items():
            s = s.replace(k, str(v))
        s = re.sub(r'\b(0x[0-9A-Fa-f]+|\d+)\b', lambda m: str(int(m.group(1), 0)), s)
        if not re.fullmatch(r'[\d\s+\-*/()]+', s):
            raise ValueError('cannot evaluate array bound: %r' % s)
        return int(eval(s))

    def parse(self, text):
        t = tokenize(strip(text))
        i = 0
        while i < len(t):
            if t[i] in ('struct', 'union') and i + 2 < len(t) and t[i + 2] == '{':
                i = self.parse_body(t, i, None)
            else:
                i += 1

    def parse_body(self, t, i, anon_name):
        kind = t[i]
        if anon_name is None:
            name = t[i + 1]
            i += 3
        else:
            name = anon_name
            i += 2
        fields = []
        while t[i] != '}':
            if t[i] in ('struct', 'union') and t[i + 1] == '{':
                # inline anonymous aggregate: struct { ... } Name;
                inner = '%s_%d' % (name, len(fields))
                j = self.parse_body(t, i, inner)
                fname = t[j]
                real = '%s_%s' % (name, fname)
                self.structs[real] = self.structs.pop(inner)
                self.order[self.order.index(inner)] = real
                fields.append((fname, real, 1, None))
                i = j + 2
                continue
            # skip member functions: tokens up to the matching '}' of their body, or ';'
            j = i
            depth = 0
            stmt = []
            is_func = False
            while True:
                tok = t[j]
                if tok == '(' and depth == 0 and not is_func and '[' not in stmt:
                    is_func = True
                if tok == '{':
                    depth += 1
                elif tok == '}':
                    if depth == 0:
                        break
                    depth -= 1
                    if depth == 0 and is_func:
                        j += 1
                        break
                elif tok == ';' and depth == 0:
                    j += 1
                    break
                stmt.append(tok)
                j += 1
            i = j
            if is_func or not stmt:
                continue
            fields.append(self.member(stmt))
        self.structs[name] = {'kind': kind, 'fields': fields}
        self.order.append(name)
        return i + 1  # past '}'

    def member(self, stmt):
        # <type> <name> [ '[' expr ']' ] [ ':' bits ]
        ctype, fname = stmt[0], stmt[1]
        count, bits = 1, None
        rest = stmt[2:]
        if rest and rest[0] == '[':
            close = rest.index(']')
            count = self.eval_expr(rest[1:close])
            rest = rest[close + 1:]
        if rest and rest[0] == ':':
            bits = int(rest[1], 0)
        return (fname, ctype, count, bits)


def layout(p, name, cache):
    """-> (size, align, [field dicts])"""
    if name in cache:
        return cache[name]
    s = p.structs[name]
    off = 0
    align = 1
    out = []
    unit = None  # (ctype, offset, bits used, unit size)
    for fname, ctype, count, bits in s['fields']:
        if ctype in PRIM:
            dt, sz = PRIM[ctype]
            al = sz
            sub = None
        else:
            sz, al, _ = layout(p, ctype, cache)
            dt = 'STRUCT'
            sub = ctype
        if s['kind'] == 'union':
            foff = 0
        elif bits is not None:
            if unit and unit[0] == ctype and unit[2] + bits <= sz * 8:
                foff = unit[1]
                bit_lo = unit[2]
                unit = (ctype, unit[1], unit[2] + bits, sz)
            else:
                off = (off + al - 1) // al * al
                foff = off
                bit_lo = 0
                unit = (ctype, off, bits, sz)
                off += sz
        else:
            unit = None
            off = (off + al - 1) // al * al
            foff = off
            off += sz * count
        align = max(align, al)
        f = {'name': fname, 'offset': foff, 'type': dt, 'count': count, 'stride': sz, 'sub': sub,
             'bit_lo': 0, 'bit_width': 0, 'ctype': ctype}
        if bits is not None:
            f['type'] = 'BITS'
            f['bit_lo'] = bit_lo if s['kind'] != 'union' else 0
            f['bit_width'] = bits
            f['base'] = dt
        # arrays of 8-bit integers read as strings when there's no subscript
        if count > 1 and dt in ('I8', 'U8'):
            if ctype in ('int8_t', 'char') or 'Name' in fname or fname in ('SearchComment',):
                f['type'] = 'CHARS'
            else:
                f['type'] = 'BYTES'
            f['elem'] = dt
        out.append(f)
    if s['kind'] == 'union':
        size = max((f['stride'] * f['count'] for f in out), default=0)
    else:
        size = off
    size = (size + align - 1) // align * align
    cache[name] = (size, align, out)
    return cache[name]


def main():
    sdk = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SDK
    enums = enum_values(open(os.path.join(sdk, 'enums.h'), encoding='utf-8', errors='replace').read())
    p = Parser(enums)
    asserts = {}
    for h in HEADERS:
        text = open(os.path.join(sdk, h), encoding='utf-8', errors='replace').read()
        p.parse(text)
        for m in re.finditer(r'static_assert\(sizeof\((\w+)\)\s*==\s*(\d+)', text):
            asserts[m.group(1)] = int(m.group(2))
    cache = {}
    for name in p.order:
        layout(p, name, cache)
    bad = []
    for name, want in asserts.items():
        got = cache[name][0]
        if got != want:
            bad.append('%s: %d, header says %d' % (name, got, want))
    for name in p.order:
        for f in cache[name][2]:
            m = re.fullmatch(r'(?:unknown|padding)([0-9A-Fa-f]{4,5})', f['name'])
            if m and int(m.group(1), 16) != f['offset']:
                bad.append('%s.%s at 0x%X' % (name, f['name'], f['offset']))
    if bad:
        sys.exit('layout mismatch:\n  ' + '\n  '.join(bad))

    names = list(p.order)
    idx = {n: i for i, n in enumerate(names)}
    lines = []
    lines.append('/* Generated by tools/gen_game_fields.py from Ashita v4 SDK plugins/sdk/ffxi/ headers. Do not edit.')
    lines.append(' * Every struct size below matches the SDK header\'s static_assert (%d checked). */' % len(asserts))
    lines.append('')
    lines.append('enum xi_game_struct_id')
    lines.append('{')
    for n in names:
        lines.append('    XI_S_%s,' % n)
    lines.append('    XI_S_COUNT')
    lines.append('};')
    lines.append('')
    lines.append('static const xi_field xi_game_fields[] = {')
    first = {}
    k = 0
    for n in names:
        first[n] = k
        lines.append('    /* %s */' % n)
        for f in cache[n][2]:
            sub = 'XI_S_%s' % f['sub'] if f['sub'] else '-1'
            lines.append('    {"%s", 0x%04X, XI_T_%s, XI_T_%s, %d, %d, %d, %d, %s},' % (
                f['name'], f['offset'], f['type'], f.get('elem', f.get('base', f['type'])),
                f['count'], f['stride'], f['bit_lo'], f['bit_width'], sub))
            k += 1
    lines.append('};')
    lines.append('')
    lines.append('/* Sizes, and offsets / array lengths of each struct\'s own fields (struct name without "_t"). */')
    for n in names:
        lines.append('#define XI_SIZE_%s 0x%X' % (n, cache[n][0]))
        short = n[:-2] if n.endswith('_t') else n
        for f in cache[n][2]:
            lines.append('#define XI_OFS_%s_%s 0x%X' % (short, f['name'], f['offset']))
            if f['count'] > 1:
                lines.append('#define XI_COUNT_%s_%s %d' % (short, f['name'], f['count']))
    lines.append('')
    lines.append('static const xi_struct xi_game_structs[XI_S_COUNT] = {')
    for n in names:
        lines.append('    {"%s", %d, %d, %d},' % (n, cache[n][0], first[n], len(cache[n][2])))
    lines.append('};')
    lines.append('')
    with open(OUT, 'w', newline='\n') as f:
        f.write('\n'.join(lines))
    print('wrote %s: %d structs, %d fields' % (os.path.relpath(OUT, ROOT), len(names), k))
    for n in ('entity_t', 'party_t', 'partymember_t', 'player_t', 'target_t', 'targetwindow_t',
              'inventory_t', 'autofollow_t', 'castbar_t', 'partystatusicons_t'):
        print('  %-20s 0x%X' % (n, cache[n][0]))


if __name__ == '__main__':
    main()
