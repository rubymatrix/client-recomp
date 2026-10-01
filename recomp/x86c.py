"""x86-32 -> C translation of one guest function.

Model (see runtime/guest.h):
  * registers and flags are C locals, loaded from Guest at entry and stored around calls/returns;
  * guest memory is accessed through rd8/16/32/64 and wr8/16/32/64 on 32-bit guest addresses;
  * every instruction that is a branch target gets a label; jumps are gotos;
  * direct calls push the return address on the guest stack and call the callee's C function,
    `ret` pops and returns; a jump to another function's entry is a tail call;
  * switches (`jmp [reg*4+table]`) read the table from guest memory and dispatch with a C switch
    over the targets the metadata lists;
  * SSE/SSE2 instructions run on the XMM registers in Guest (g->xmm, runtime/sse.h): the source is
    copied to a temporary X128 S and one helper does the lane work (sse());
  * x87 instructions are decoded from their opcode bytes (not the mnemonic: disassemblers
    disagree on FSUBP/FSUBRP naming) and run on doubles with the control word's RC and PC.

Anything not handled becomes RT_UNIMPL(addr, "text"), which traps at run time; the recompiler
counts them so coverage can be measured over the whole binary.
"""
import re

import capstone
from capstone import x86_const as X

R32 = ('eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi')
R16 = {'ax': 'eax', 'cx': 'ecx', 'dx': 'edx', 'bx': 'ebx', 'sp': 'esp', 'bp': 'ebp', 'si': 'esi', 'di': 'edi'}
R8L = {'al': 'eax', 'cl': 'ecx', 'dl': 'edx', 'bl': 'ebx'}
R8H = {'ah': 'eax', 'ch': 'ecx', 'dh': 'edx', 'bh': 'ebx'}
UT = {8: 'uint8_t', 16: 'uint16_t', 32: 'uint32_t', 64: 'uint64_t'}
ST_ = {8: 'int8_t', 16: 'int16_t', 32: 'int32_t', 64: 'int64_t'}

CC = {
    'o': 'of', 'no': '!of', 'b': 'cf', 'ae': '!cf', 'e': 'zf', 'ne': '!zf', 'be': '(cf || zf)', 'a': '(!cf && !zf)',
    's': 'sf', 'ns': '!sf', 'p': 'pf', 'np': '!pf', 'l': '(sf != of)', 'ge': '(sf == of)',
    'le': '(zf || sf != of)', 'g': '(!zf && sf == of)',
}


class Unsupported(Exception):
    pass


# Emitted x87 statements that take the whole x87 state from g (runtime/guest.h): in a function with
# the stack in locals, g is brought up to date before them and read back after.
X87_WHOLE = ('fenv_load(', 'fenv_store(', 'fsave(', 'frstor(', 'fxam(', 'g->top')


def x87_locals(lines):
    """A function's text with its x87 stack in locals (guest.h, "x87 in locals") instead of g->st."""
    out = []
    for line in lines:
        code = line.lstrip()
        if not code.startswith('/*') and any(k in code for k in X87_WHOLE):
            out.append(line[:len(line) - len(code)] + 'X87_STORE; ' + code + ' X87_LOAD;')
            continue
        s = line.replace('REGS_DECL;', 'REGS_DECL; X87_DECL;').replace('REGS_LOAD;', 'REGS_LOAD; X87_LOAD;')
        s = s.replace('REGS_STORE;', 'REGS_STORE; X87_STORE;')
        s = re.sub(r'\bST\(([0-7])\)', r'x87_s\1', s)
        s = s.replace('fpush(g, ', 'X87_PUSH(').replace('fpop(g)', 'X87_POP()').replace('fr(g, ', 'fr_cw(x87_cw, ')
        for f in ('frnd', 'fist16', 'fist32', 'fist64'):
            s = s.replace(f + '(g, ', f + '_cw(x87_cw, ')
        s = s.replace('fstsw(g)', 'fstsw_top(g, x87_top)').replace('g->fcw', 'x87_cw')
        out.append(s)
    return out


def mask(bits):
    return (1 << bits) - 1


def hexu(v, bits=32):
    return '0x%Xu' % (v & mask(bits))


class FunctionTranslator:
    def __init__(self, ctx, entry, ranges):
        self.ctx = ctx            # Program: image bytes, function entries, switches
        self.entry = entry
        self.ranges = ranges
        self.lines = []
        self.unimpl = []          # (addr, text)
        self.insns = []
        self.labels = set()
        self.uses_x87 = False     # then the function keeps the x87 stack in locals (x87_locals)

    # ------------------------------------------------------------------ decoding

    def decode(self):
        md = self.ctx.md
        for lo, hi in self.ranges:
            code = self.ctx.read(lo, hi - lo)
            for ins in md.disasm(code, lo):
                self.insns.append(ins)
        self.addrs = set(i.address for i in self.insns)
        # The metadata's ranges come from Ghidra, which stops a body where another function begins.
        # Code this function jumps to beyond its ranges (not another function's entry) is part of
        # it: decode it by following control flow, so the translation never depends on those cuts
        # (e.g. 0x10320813 jumps to 0x10320917, past a function Ghidra started inside its body).
        work = [t for i in self.insns for t in self.jump_targets(i)]
        while work:
            pc = work.pop()
            while pc not in self.addrs and pc not in self.ctx.entries:
                found = list(md.disasm(self.ctx.read(pc, 16), pc, count=1))
                if not found:
                    break
                ins = found[0]
                self.insns.append(ins)
                self.addrs.add(pc)
                work.extend(self.jump_targets(ins))
                if ins.mnemonic in ('ret', 'jmp', 'int3') or ins.mnemonic.startswith('ret'):
                    break
                pc = ins.address + ins.size
        self.insns.sort(key=lambda i: i.address)

    def jump_targets(self, ins):
        """Direct intra-procedural control transfers out of an instruction, and the return point of
        a call: Ghidra ends a body after a call it believes never returns, and is sometimes wrong
        (ffxivgame.exe 0x9e4a69 calls free(), 0x9d5c88, and its body stopped there), so the code
        after every call is decoded as this function's unless another function starts there."""
        mn = ins.mnemonic
        out = []
        if mn == 'call':
            return [ins.address + ins.size]
        if mn == 'jmp' or (mn[0] == 'j' and mn[1:] in CC) or mn in ('jecxz', 'loop', 'loope', 'loopne'):
            if ins.operands and ins.operands[0].type == X.X86_OP_IMM:
                out.append(ins.operands[0].imm & 0xFFFFFFFF)
            elif mn == 'jmp':
                sw = self.ctx.switches.get(ins.address)
                if sw is None:
                    sw = self.ctx.infer_switch(ins)
                out.extend(sw or ())
        return out

    def inside(self, a):
        return a in self.addrs

    # ------------------------------------------------------------------ operands

    def reg(self, ins, r):
        name = ins.reg_name(r)
        if name in R32:
            return name, 'd', 32
        if name in R16:
            return R16[name], 'w', 16
        if name in R8L:
            return R8L[name], 'l', 8
        if name in R8H:
            return R8H[name], 'h', 8
        raise Unsupported('register %s' % name)

    def addr(self, ins, op, lea=False):
        m = op.mem
        parts = []
        if m.base:
            b, kind, bits = self.reg(ins, m.base)
            if bits != 32:
                raise Unsupported('16-bit addressing')
            parts.append(b)
        if m.index:
            i, kind, bits = self.reg(ins, m.index)
            parts.append('%s * %d' % (i, m.scale) if m.scale != 1 else i)
        if m.disp:
            parts.append(hexu(m.disp) + (' + RD' if self.reloc_disp(ins) else ''))
        if not lea and m.segment == X.X86_REG_FS:
            parts.append('g->fs_base')
        elif not lea and m.segment not in (0, X.X86_REG_DS, X.X86_REG_ES, X.X86_REG_SS, X.X86_REG_CS):
            raise Unsupported('segment %s' % ins.reg_name(m.segment))
        return '(uint32_t)(%s)' % (' + '.join(parts) if parts else '0u')

    def reloc_disp(self, ins):
        """The displacement is an absolute image address (relocated at load time)."""
        return ins.disp_offset > 0 and ins.address + ins.disp_offset in self.ctx.relocs

    def reloc_imm(self, ins):
        """The immediate is an absolute image address (push offset f, mov eax, offset g, cmp r, offset h)."""
        return ins.imm_offset > 0 and ins.address + ins.imm_offset in self.ctx.relocs

    def bits_of(self, op):
        return op.size * 8

    def rd(self, ins, op, bits=None):
        bits = bits or self.bits_of(op)
        if op.type == X.X86_OP_REG:
            b, kind, rb = self.reg(ins, op.reg)
            return {'d': b, 'w': '(uint16_t)%s' % b, 'l': '(uint8_t)%s' % b, 'h': '(uint8_t)(%s >> 8)' % b}[kind]
        if op.type == X.X86_OP_IMM:
            return '(%s + RD)' % hexu(op.imm, bits) if self.reloc_imm(ins) else hexu(op.imm, bits)
        if op.type == X.X86_OP_MEM:
            return 'rd%d(A)' % bits
        raise Unsupported('operand type %d' % op.type)

    def wr(self, ins, op, expr):
        bits = self.bits_of(op)
        if op.type == X.X86_OP_REG:
            b, kind, rb = self.reg(ins, op.reg)
            if kind == 'd':
                return '%s = (uint32_t)(%s);' % (b, expr)
            if kind == 'w':
                return '%s = (%s & 0xFFFF0000u) | (uint16_t)(%s);' % (b, b, expr)
            if kind == 'l':
                return '%s = (%s & 0xFFFFFF00u) | (uint8_t)(%s);' % (b, b, expr)
            return '%s = (%s & 0xFFFF00FFu) | ((uint32_t)(uint8_t)(%s) << 8);' % (b, b, expr)
        if op.type == X.X86_OP_MEM:
            return 'wr%d(A, (%s)(%s));' % (bits, UT[bits], expr)
        raise Unsupported('write to operand type %d' % op.type)

    def mem_prologue(self, ins):
        for op in ins.operands:
            if op.type == X.X86_OP_MEM:
                return ['uint32_t A = %s;' % self.addr(ins, op)]
        return []

    # ------------------------------------------------------------------ flags

    @staticmethod
    def szp(r, bits):
        return ['zf = (%s)%s == 0;' % (UT[bits], r), 'sf = ((%s) >> %d) & 1u;' % (r, bits - 1), 'pf = PARITY(%s);' % r]

    def flags_add(self, a, b, r, bits, carry=None):
        out = []
        if carry is None:
            out.append('cf = (%s)%s < (%s)%s;' % (UT[bits], r, UT[bits], a))
        else:
            out.append('cf = (unsigned)((((uint64_t)%s + (uint64_t)%s + %s) >> %d) & 1u);' % (a, b, carry, bits))
        out.append('of = ((((%s) ^ (%s)) & ((%s) ^ (%s))) >> %d) & 1u;' % (a, r, b, r, bits - 1))
        out.append('af = (((%s) ^ (%s) ^ (%s)) >> 4) & 1u;' % (a, b, r))
        return out + self.szp(r, bits)

    def flags_sub(self, a, b, r, bits, borrow=None):
        out = []
        if borrow is None:
            out.append('cf = (%s)%s < (%s)%s;' % (UT[bits], a, UT[bits], b))
        else:
            out.append('cf = (uint64_t)%s < (uint64_t)%s + %s;' % (a, b, borrow))
        out.append('of = ((((%s) ^ (%s)) & ((%s) ^ (%s))) >> %d) & 1u;' % (a, b, a, r, bits - 1))
        out.append('af = (((%s) ^ (%s) ^ (%s)) >> 4) & 1u;' % (a, b, r))
        return out + self.szp(r, bits)

    def flags_logic(self, r, bits):
        return ['cf = 0; of = 0; af = 0;'] + self.szp(r, bits)

    # ------------------------------------------------------------------ control flow helpers

    def goto(self, target, from_addr):
        """C statement(s) transferring control to a guest address from inside this function."""
        if self.inside(target):
            self.labels.add(target)
            if from_addr is not None and target <= from_addr:
                # A backward jump closes a loop: a safepoint, so a guest thread spinning on a
                # variable still lets the others run under the guest lock.
                return '{ RT_SAFEPOINT; goto L_%08x; }' % target
            return 'goto L_%08x;' % target
        if target in self.ctx.entries:
            self.ctx.referenced.add(target)
            return '{ REGS_STORE; %s%08x(g); return; }' % (self.ctx.prefix, target)
        return 'RT_BADJUMP(%s);' % hexu(target)

    def call(self, target, ret_addr):
        self.ctx.referenced.add(target)
        return ['esp -= 4; wr32(esp, %s + RD);' % hexu(ret_addr), 'REGS_STORE; %s%08x(g); REGS_LOAD;' % (self.ctx.prefix, target)]

    # ------------------------------------------------------------------ translation

    def translate(self):
        self.decode()
        body = []
        prev_falls_to = None
        for k, ins in enumerate(self.insns):
            if prev_falls_to is not None and prev_falls_to != ins.address:
                body.append('    ' + self.goto(prev_falls_to, None))
            body.append(('L_%08x:' % ins.address, ins))
            try:
                stmts, falls = self.one(ins)
            except Unsupported as e:
                text = '%s %s' % (ins.mnemonic, ins.op_str)
                self.unimpl.append((ins.address, ins.mnemonic, str(e)))
                stmts, falls = ['RT_UNIMPL(%s, "%s");' % (hexu(ins.address), text.replace('"', "'"))], False
            hook = self.ctx.hooks.get(ins.address)
            if hook:
                # A host hook point (recomp.py --hooks): the host sees and may change the guest's
                # state here, before this instruction runs; unset, it costs one test.
                self.ctx.hooked.add(ins.address)
                body.append('    if (rt_hook_%s) { REGS_STORE; rt_hook_%s(g); REGS_LOAD; }' % (hook, hook))
            body.append('    /* %08x: %s %s */' % (ins.address, ins.mnemonic, ins.op_str))
            if stmts:  # each instruction in its own block, so its temporaries (A, t, ...) are local to it
                body.append('    {')
                for s in stmts:
                    body.append('        ' + s)
                body.append('    }')
            prev_falls_to = ins.address + ins.size if falls else None
        if prev_falls_to is not None:
            body.append('    ' + self.goto(prev_falls_to, None))

        out = ['void %s%08x(Guest* g)' % (self.ctx.prefix, self.entry), '{', '    REGS_DECL;', '    REGS_LOAD;']
        if self.insns and self.insns[0].address != self.entry:
            out.append('    goto L_%08x;' % self.entry)
            self.labels.add(self.entry)
        for item in body:
            if isinstance(item, tuple):
                label, ins = item
                if ins.address in self.labels:
                    out.append(label + ' ;')
            else:
                out.append(item)
        out.append('}')
        return x87_locals(out) if self.uses_x87 else out

    def one(self, ins):
        """Returns (C statements, falls_through)."""
        mn = ins.mnemonic
        prefix = None
        if ' ' in mn:
            prefix, mn = mn.split(' ', 1)
            if prefix == 'lock':
                prefix = None  # one guest thread runs at a time (guest lock), so lock is implicit
        ops = ins.operands
        nxt = ins.address + ins.size
        b0 = ins.bytes[0]

        # x87 by opcode
        opc = self.x87_opcode(ins)
        if opc is not None:
            self.uses_x87 = True
            return self.x87(ins, opc), True

        if mn in ('nop', 'wait', 'fwait', 'pause'):
            return [], True
        if mn == 'int3':
            return ['RT_TRAP(%s);' % hexu(ins.address)], False

        # string instructions
        if mn[:4] in ('movs', 'stos', 'lods', 'scas', 'cmps') and len(mn) == 5 and mn[4] in 'bwd' \
                and all(o.type != X.X86_OP_REG or self.ins_reg_is_gpr(ins, o) for o in ops):
            return self.string(ins, mn, prefix), True
        if prefix in ('rep', 'repz') and mn == 'ret':
            prefix = None  # `rep ret`: AMD's branch-predictor idiom, an ordinary ret
        if prefix is not None:
            raise Unsupported('prefix %s' % prefix)

        sse = self.sse(ins, mn, ops)
        if sse is not None:
            return sse, True

        pro = self.mem_prologue(ins)

        if mn in ('mov', 'movzx', 'movsx'):
            d, s = ops
            sb, db = self.bits_of(s), self.bits_of(d)
            v = self.rd(ins, s)
            if mn == 'movsx':
                v = '(%s)(%s)(%s)' % (UT[db], ST_[db], '(%s)%s' % (ST_[sb], v))
            return pro + [self.wr(ins, d, v)], True
        if mn == 'lea':
            d, s = ops
            return [self.wr(ins, d, self.addr(ins, s, lea=True))], True
        if mn == 'xchg':
            a, b = ops
            return pro + ['{ %s t = %s;' % (UT[self.bits_of(a)], self.rd(ins, a)), self.wr(ins, a, self.rd(ins, b)),
                          self.wr(ins, b, 't') + ' }'], True
        if mn in ('add', 'adc', 'sub', 'sbb', 'cmp', 'and', 'or', 'xor', 'test'):
            d, s = ops
            bits = self.bits_of(d)
            T = UT[bits]
            a, b = 'a', 'b'
            st = pro + ['{', '%s a = %s; %s b = (%s)%s;' % (T, self.rd(ins, d), T, T, self.rd(ins, s, bits))]
            if mn in ('adc', 'sbb'):
                st.append('unsigned c = cf;')
            expr = {'add': 'a + b', 'adc': 'a + b + c', 'sub': 'a - b', 'sbb': 'a - b - c', 'cmp': 'a - b',
                    'and': 'a & b', 'or': 'a | b', 'xor': 'a ^ b', 'test': 'a & b'}[mn]
            st.append('%s r = (%s)(%s);' % (T, T, expr))
            if mn in ('add', 'adc'):
                st += self.flags_add(a, b, 'r', bits, 'c' if mn == 'adc' else None)
            elif mn in ('sub', 'sbb', 'cmp'):
                st += self.flags_sub(a, b, 'r', bits, 'c' if mn == 'sbb' else None)
            else:
                st += self.flags_logic('r', bits)
            if mn not in ('cmp', 'test'):
                st.append(self.wr(ins, d, 'r'))
            st.append('}')
            return st, True
        if mn in ('inc', 'dec'):
            d, = ops
            bits = self.bits_of(d)
            T = UT[bits]
            sign = 1 << (bits - 1)
            if mn == 'inc':
                return pro + ['{ %s a = %s; %s r = (%s)(a + 1u);' % (T, self.rd(ins, d), T, T),
                              'of = r == %s; af = (r & 0xFu) == 0;' % hexu(sign, bits)] + self.szp('r', bits) + \
                    [self.wr(ins, d, 'r') + ' }'], True
            return pro + ['{ %s a = %s; %s r = (%s)(a - 1u);' % (T, self.rd(ins, d), T, T),
                          'of = a == %s; af = (r & 0xFu) == 0xFu;' % hexu(sign, bits)] + self.szp('r', bits) + \
                [self.wr(ins, d, 'r') + ' }'], True
        if mn == 'neg':
            d, = ops
            bits = self.bits_of(d)
            T = UT[bits]
            return pro + ['{ %s a = %s; %s r = (%s)(0u - a);' % (T, self.rd(ins, d), T, T),
                          'cf = a != 0; of = a == %s; af = (a & 0xFu) != 0;' % hexu(1 << (bits - 1), bits)] + \
                self.szp('r', bits) + [self.wr(ins, d, 'r') + ' }'], True
        if mn == 'not':
            d, = ops
            return pro + [self.wr(ins, d, '~%s' % self.rd(ins, d))], True
        if mn in ('shl', 'sal', 'shr', 'sar', 'rol', 'ror'):
            return pro + self.shift(ins, mn), True
        if mn in ('rcl', 'rcr'):
            d = ops[0]
            bits = self.bits_of(d)
            T = UT[bits]
            cnt = self.rd(ins, ops[1], 8) if len(ops) > 1 else '1u'
            if mn == 'rcr':
                step = 'unsigned o = r & 1u; r = (%s)((r >> 1) | ((uint32_t)cf << %d)); cf = o;' % (T, bits - 1)
                ofx = 'of = ((r >> %d) ^ (r >> %d)) & 1u;' % (bits - 1, bits - 2)
            else:
                step = 'unsigned o = (r >> %d) & 1u; r = (%s)((r << 1) | cf); cf = o;' % (bits - 1, T)
                ofx = 'of = ((r >> %d) & 1u) ^ cf;' % (bits - 1)
            return pro + ['{ unsigned c = ((unsigned)(%s) & 31u) %% %du; %s r = %s; unsigned i;' % (cnt, bits + 1, T, self.rd(ins, d)),
                          'for (i = 0; i < c; i++) { %s }' % step, 'if (c) { %s }' % ofx, self.wr(ins, d, 'r') + ' }'], True
        if mn in ('shld', 'shrd'):
            return pro + self.shiftd(ins, mn), True
        if mn in ('mul', 'imul', 'div', 'idiv'):
            return pro + self.muldiv(ins, mn), True
        if mn == 'cdq':
            return ['edx = (uint32_t)((int32_t)eax >> 31);'], True
        if mn == 'cwde':
            return ['eax = (uint32_t)(int32_t)(int16_t)eax;'], True
        if mn == 'cbw':
            return ['eax = (eax & 0xFFFF0000u) | (uint16_t)(int16_t)(int8_t)eax;'], True
        if mn == 'cwd':
            return ['edx = (edx & 0xFFFF0000u) | ((eax & 0x8000u) ? 0xFFFFu : 0u);'], True
        if mn == 'push':
            s, = ops
            if self.bits_of(s) != 32 and s.type != X.X86_OP_IMM:
                raise Unsupported('16-bit push')
            return pro + ['{ uint32_t v = %s; esp -= 4; wr32(esp, v); }' % self.rd(ins, s, 32)], True
        if mn == 'pop':
            d, = ops
            if self.bits_of(d) != 32:
                raise Unsupported('16-bit pop')
            if d.type == X.X86_OP_MEM:
                # the address is computed with esp already incremented
                return ['{ uint32_t v = rd32(esp); esp += 4; uint32_t A = %s; wr32(A, v); }' % self.addr(ins, d)], True
            return ['{ uint32_t v = rd32(esp); esp += 4; %s }' % self.wr(ins, d, 'v')], True
        if mn == 'pushal':
            return ['{ uint32_t s = esp; esp -= 32; wr32(esp + 28, eax); wr32(esp + 24, ecx); wr32(esp + 20, edx); '
                    'wr32(esp + 16, ebx); wr32(esp + 12, s); wr32(esp + 8, ebp); wr32(esp + 4, esi); wr32(esp, edi); }'], True
        if mn == 'popal':
            return ['edi = rd32(esp); esi = rd32(esp + 4); ebp = rd32(esp + 8); ebx = rd32(esp + 16); '
                    'edx = rd32(esp + 20); ecx = rd32(esp + 24); eax = rd32(esp + 28); esp += 32;'], True
        if mn == 'pushfd':
            return ['{ uint32_t f = cf | 2u | (pf << 2) | (af << 4) | (zf << 6) | (sf << 7) | (df << 10) | (of << 11); '
                    'esp -= 4; wr32(esp, f); }'], True
        if mn == 'popfd':
            return ['{ uint32_t f = rd32(esp); esp += 4; cf = f & 1u; pf = (f >> 2) & 1u; af = (f >> 4) & 1u; '
                    'zf = (f >> 6) & 1u; sf = (f >> 7) & 1u; df = (f >> 10) & 1u; of = (f >> 11) & 1u; }'], True
        if mn == 'lahf':
            return ['eax = (eax & 0xFFFF00FFu) | ((sf << 15) | (zf << 14) | (af << 12) | (pf << 10) | (1u << 9) | (cf << 8));'], True
        if mn == 'sahf':
            return ['{ uint32_t h = (eax >> 8) & 0xFFu; cf = h & 1u; pf = (h >> 2) & 1u; af = (h >> 4) & 1u; '
                    'zf = (h >> 6) & 1u; sf = (h >> 7) & 1u; }'], True
        if mn == 'enter':
            size, level = ops[0].imm & 0xFFFF, ops[1].imm & 31
            if level:
                raise Unsupported('enter with nesting level')
            return ['esp -= 4; wr32(esp, ebp); ebp = esp; esp -= %du;' % size], True
        if mn == 'leave':
            return ['esp = ebp; ebp = rd32(esp); esp += 4;'], True
        if mn in ('clc', 'stc', 'cmc', 'cld', 'std'):
            return [{'clc': 'cf = 0;', 'stc': 'cf = 1;', 'cmc': 'cf ^= 1u;', 'cld': 'df = 0;', 'std': 'df = 1;'}[mn]], True
        if mn.startswith('set') and mn[3:] in CC:
            d, = ops
            return pro + [self.wr(ins, d, '(%s) ? 1u : 0u' % CC[mn[3:]])], True
        if mn.startswith('cmov') and mn[4:] in CC:
            d, s = ops
            return pro + ['if (%s) { %s }' % (CC[mn[4:]], self.wr(ins, d, self.rd(ins, s)))], True
        if mn in ('bt', 'bts', 'btr', 'btc'):
            return pro + self.bittest(ins, mn), True
        if mn in ('bsf', 'bsr'):
            d, s = ops
            if self.bits_of(d) != 32:
                raise Unsupported('16-bit bsf/bsr')
            loop = 'for (i = 0; !((v >> i) & 1u); i++) ;' if mn == 'bsf' else 'for (i = 31; !((v >> i) & 1u); i--) ;'
            return pro + ['{ uint32_t v = %s; zf = v == 0; if (v) { unsigned i; %s %s } }' % (self.rd(ins, s), loop, self.wr(ins, d, 'i'))], True
        if mn == 'bswap':
            d, = ops
            b = self.rd(ins, d)
            return ['%s = (%s >> 24) | ((%s >> 8) & 0xFF00u) | ((%s << 8) & 0xFF0000u) | (%s << 24);' % (b, b, b, b, b)], True
        if mn == 'xadd':
            d, s = ops
            bits = self.bits_of(d)
            T = UT[bits]
            return pro + ['{ %s a = %s; %s b = %s; %s r = (%s)(a + b);' % (T, self.rd(ins, d), T, self.rd(ins, s), T, T)] + \
                self.flags_add('a', 'b', 'r', bits) + [self.wr(ins, s, 'a'), self.wr(ins, d, 'r') + ' }'], True
        if mn == 'cmpxchg':
            d, s = ops
            bits = self.bits_of(d)
            T = UT[bits]
            acc = {8: '(uint8_t)eax', 16: '(uint16_t)eax', 32: 'eax'}[bits]
            acc_w = {8: 'eax = (eax & 0xFFFFFF00u) | (uint8_t)v;', 16: 'eax = (eax & 0xFFFF0000u) | (uint16_t)v;', 32: 'eax = v;'}[bits]
            return pro + ['{ %s a = %s; %s v = %s; %s r = (%s)(a - v);' % (T, acc, T, self.rd(ins, d), T, T)] + \
                self.flags_sub('a', 'v', 'r', bits) + \
                ['if (zf) { %s } else { %s } }' % (self.wr(ins, d, self.rd(ins, s)), acc_w)], True
        if mn == 'xlatb':
            return ['eax = (eax & 0xFFFFFF00u) | rd8(ebx + (eax & 0xFFu));'], True
        if mn == 'cpuid':
            return ['REGS_STORE; rt_cpuid(g); REGS_LOAD;'], True
        if mn == 'rdtsc':
            return ['REGS_STORE; rt_rdtsc(g); REGS_LOAD;'], True

        # control flow
        if mn == 'jmp':
            t, = ops
            if t.type == X.X86_OP_IMM:
                return [self.goto(t.imm & 0xFFFFFFFF, ins.address)], False
            sw = self.ctx.switches.get(ins.address)
            if sw is not None and t.type == X.X86_OP_MEM:
                cases = []
                for tgt in sw:
                    cases.append('case %s: %s' % (hexu(tgt), self.goto(tgt, ins.address)))
                return pro + ['switch (rd32(A) - RD) {'] + ['    ' + c for c in cases] + \
                    ['    default: RT_BADJUMP(%s);' % hexu(ins.address), '}'], False
            return pro + ['{ uint32_t t = %s; REGS_STORE; rt_call_indirect(g, t); return; }' % self.rd(ins, t, 32)], False
        if mn[0] == 'j' and mn[1:] in CC:
            t, = ops
            return ['if (%s) %s' % (CC[mn[1:]], self.goto(t.imm & 0xFFFFFFFF, ins.address))], True
        if mn == 'jecxz':
            t, = ops
            return ['if (ecx == 0) %s' % self.goto(t.imm & 0xFFFFFFFF, ins.address)], True
        if mn in ('loop', 'loope', 'loopne'):
            t, = ops
            cond = {'loop': 'ecx != 0', 'loope': 'ecx != 0 && zf', 'loopne': 'ecx != 0 && !zf'}[mn]
            return ['ecx--;', 'if (%s) %s' % (cond, self.goto(t.imm & 0xFFFFFFFF, ins.address))], True
        if mn == 'call':
            t, = ops
            if t.type == X.X86_OP_IMM:
                tgt = t.imm & 0xFFFFFFFF
                if tgt == nxt:  # call $+5 ; pop r  (get EIP)
                    return ['esp -= 4; wr32(esp, %s + RD);' % hexu(nxt)], True
                if tgt not in self.ctx.entries:
                    # A call to code no function starts at (an SEH __finally block, or code past
                    # a body Ghidra cut short): it becomes an entry of its own, translated too.
                    end = self.ctx.code_until_ret(tgt)
                    if end:
                        self.ctx.functions[tgt] = [[tgt, end]]
                        self.ctx.entries.add(tgt)
                if tgt in self.ctx.entries:
                    return self.call(tgt, nxt), True
                return ['RT_BADJUMP(%s);' % hexu(tgt)], False
            return pro + ['{ uint32_t t = %s; esp -= 4; wr32(esp, %s + RD); REGS_STORE; rt_call_indirect(g, t); REGS_LOAD; }'
                          % (self.rd(ins, t, 32), hexu(nxt))], True
        if mn == 'ret':
            n = ops[0].imm if ops else 0
            return ['esp += %d; REGS_STORE; return;' % (4 + n)], False

        raise Unsupported(mn)

    def ins_reg_is_gpr(self, ins, op):
        try:
            self.reg(ins, op.reg)
            return True
        except Unsupported:
            return False

    # ------------------------------------------------------------------ groups

    def shift(self, ins, mn):
        d = ins.operands[0]
        bits = self.bits_of(d)
        T, S = UT[bits], ST_[bits]
        cnt = self.rd(ins, ins.operands[1], 8) if len(ins.operands) > 1 else '1u'
        st = ['{ unsigned c = (unsigned)(%s) & 31u; if (c) { %s a = %s; %s r;' % (cnt, T, self.rd(ins, d), T)]
        if mn in ('shl', 'sal'):
            st += ['r = (%s)((uint32_t)a << c);' % T,
                   'cf = c <= %d ? ((uint32_t)a >> (%d - c)) & 1u : 0u;' % (bits, bits),
                   'of = ((r >> %d) & 1u) ^ cf;' % (bits - 1)] + self.szp('r', bits)
        elif mn == 'shr':
            st += ['r = (%s)((uint32_t)a >> c);' % T, 'cf = ((uint32_t)a >> (c - 1)) & 1u;',
                   'of = (a >> %d) & 1u;' % (bits - 1)] + self.szp('r', bits)
        elif mn == 'sar':
            st += ['{ unsigned k = c < %du ? c : %du; r = (%s)((int32_t)(%s)a >> k);' % (bits, bits - 1, T, S),
                   'cf = (unsigned)(((int32_t)(%s)a >> (k - (c < %du ? 1u : 0u))) & 1); }' % (S, bits),
                   'of = 0;'] + self.szp('r', bits)
        elif mn == 'rol':
            st += ['{ unsigned k = c %% %du; r = k ? (%s)(((uint32_t)a << k) | ((uint32_t)a >> (%d - k))) : a; }' % (bits, T, bits),
                   'cf = r & 1u; of = ((r >> %d) & 1u) ^ cf;' % (bits - 1)]
        elif mn == 'ror':
            st += ['{ unsigned k = c %% %du; r = k ? (%s)(((uint32_t)a >> k) | ((uint32_t)a << (%d - k))) : a; }' % (bits, T, bits),
                   'cf = (r >> %d) & 1u; of = ((r >> %d) ^ (r >> %d)) & 1u;' % (bits - 1, bits - 1, bits - 2)]
        st += [self.wr(ins, d, 'r'), '} }']
        return st

    def shiftd(self, ins, mn):
        d, s, n = ins.operands
        if self.bits_of(d) != 32:
            raise Unsupported('16-bit shld/shrd')
        st = ['{ unsigned c = (unsigned)(%s) & 31u; if (c) { uint32_t a = %s, b = %s, r;' % (self.rd(ins, n, 8), self.rd(ins, d), self.rd(ins, s))]
        if mn == 'shld':
            st += ['r = (a << c) | (b >> (32 - c));', 'cf = (a >> (32 - c)) & 1u;', 'of = ((r ^ a) >> 31) & 1u;']
        else:
            st += ['r = (a >> c) | (b << (32 - c));', 'cf = (a >> (c - 1)) & 1u;', 'of = ((r ^ a) >> 31) & 1u;']
        st += self.szp('r', 32) + [self.wr(ins, d, 'r'), '} }']
        return st

    def muldiv(self, ins, mn):
        ops = ins.operands
        if mn == 'imul' and len(ops) >= 2:
            d = ops[0]
            bits = self.bits_of(d)
            S = ST_[bits]
            a = self.rd(ins, ops[1]) if len(ops) == 3 else self.rd(ins, d)
            b = self.rd(ins, ops[2], bits) if len(ops) == 3 else self.rd(ins, ops[1], bits)
            return ['{ int64_t p = (int64_t)(%s)%s * (int64_t)(%s)%s; %s r = (%s)p;' % (S, a, S, b, S, S),
                    'cf = of = p != (int64_t)r;', self.wr(ins, d, '(%s)r' % UT[bits]) + ' }']
        s, = ops
        bits = self.bits_of(s)
        v = self.rd(ins, s)
        if bits == 32:
            if mn == 'mul':
                return ['{ uint64_t p = (uint64_t)eax * (uint64_t)%s; eax = (uint32_t)p; edx = (uint32_t)(p >> 32); cf = of = edx != 0; }' % v]
            if mn == 'imul':
                return ['{ int64_t p = (int64_t)(int32_t)eax * (int64_t)(int32_t)%s; eax = (uint32_t)p; edx = (uint32_t)((uint64_t)p >> 32); '
                        'cf = of = p != (int64_t)(int32_t)eax; }' % v]
            if mn == 'div':
                return ['{ uint64_t n = ((uint64_t)edx << 32) | eax; uint32_t d = %s; if (!d || n / d > 0xFFFFFFFFull) RT_DIVIDE(%s); '
                        'eax = (uint32_t)(n / d); edx = (uint32_t)(n %% d); }' % (v, hexu(ins.address))]
            return ['{ int64_t n = (int64_t)(((uint64_t)edx << 32) | eax); int32_t d = (int32_t)%s; '
                    'if (!d || (n == INT64_MIN && d == -1)) RT_DIVIDE(%s); { int64_t q = n / d; '
                    'if (q > INT32_MAX || q < INT32_MIN) RT_DIVIDE(%s); eax = (uint32_t)(int32_t)q; edx = (uint32_t)(int32_t)(n %% d); } }'
                    % (v, hexu(ins.address), hexu(ins.address))]
        if bits == 16:
            if mn == 'mul':
                return ['{ uint32_t p = (uint32_t)(uint16_t)eax * (uint32_t)%s; eax = (eax & 0xFFFF0000u) | (p & 0xFFFFu); '
                        'edx = (edx & 0xFFFF0000u) | (p >> 16); cf = of = (p >> 16) != 0; }' % v]
            if mn == 'imul':
                return ['{ int32_t p = (int32_t)(int16_t)eax * (int32_t)(int16_t)%s; eax = (eax & 0xFFFF0000u) | ((uint32_t)p & 0xFFFFu); '
                        'edx = (edx & 0xFFFF0000u) | (((uint32_t)p >> 16) & 0xFFFFu); cf = of = p != (int32_t)(int16_t)p; }' % v]
            if mn == 'div':
                return ['{ uint32_t n = ((edx & 0xFFFFu) << 16) | (eax & 0xFFFFu); uint32_t d = %s; if (!d || n / d > 0xFFFFu) RT_DIVIDE(%s); '
                        'eax = (eax & 0xFFFF0000u) | (n / d); edx = (edx & 0xFFFF0000u) | (n %% d); }' % (v, hexu(ins.address))]
            return ['{ int32_t n = (int32_t)(((edx & 0xFFFFu) << 16) | (eax & 0xFFFFu)); int32_t d = (int16_t)%s; '
                    'if (!d) RT_DIVIDE(%s); { int32_t q = n / d; if (q > 32767 || q < -32768) RT_DIVIDE(%s); '
                    'eax = (eax & 0xFFFF0000u) | ((uint32_t)q & 0xFFFFu); edx = (edx & 0xFFFF0000u) | ((uint32_t)(n %% d) & 0xFFFFu); } }'
                    % (v, hexu(ins.address), hexu(ins.address))]
        # 8-bit
        if mn == 'mul':
            return ['{ uint32_t p = (uint32_t)(uint8_t)eax * (uint32_t)%s; eax = (eax & 0xFFFF0000u) | (p & 0xFFFFu); cf = of = (p >> 8) != 0; }' % v]
        if mn == 'imul':
            return ['{ int32_t p = (int32_t)(int8_t)eax * (int32_t)(int8_t)%s; eax = (eax & 0xFFFF0000u) | ((uint32_t)p & 0xFFFFu); '
                    'cf = of = p != (int32_t)(int8_t)p; }' % v]
        if mn == 'div':
            return ['{ uint32_t n = eax & 0xFFFFu; uint32_t d = %s; if (!d || n / d > 0xFFu) RT_DIVIDE(%s); '
                    'eax = (eax & 0xFFFF0000u) | ((n %% d) << 8) | (n / d); }' % (v, hexu(ins.address))]
        return ['{ int32_t n = (int16_t)eax; int32_t d = (int8_t)%s; if (!d) RT_DIVIDE(%s); { int32_t q = n / d; '
                'if (q > 127 || q < -128) RT_DIVIDE(%s); eax = (eax & 0xFFFF0000u) | (((uint32_t)(n %% d) & 0xFFu) << 8) | ((uint32_t)q & 0xFFu); } }'
                % (v, hexu(ins.address), hexu(ins.address))]

    def bittest(self, ins, mn):
        d, s = ins.operands
        if d.type == X.X86_OP_MEM and s.type != X.X86_OP_IMM:
            # Bit string: the register offset is signed and not limited to the operand, so it
            # addresses the byte at A + (offset >> 3), bit offset & 7 (byte granularity has the
            # same effect as the CPU's dword access). CRT character-set builders use this
            # (`bts dword ptr [esp], eax` at 0x10322c0b).
            sb = self.bits_of(s)
            off = '(int32_t)(%s)%s' % (ST_[sb], self.rd(ins, s))
            st = ['{ int32_t o = %s; uint32_t B = A + (uint32_t)(o >> 3); unsigned n = (unsigned)o & 7u; uint8_t v = rd8(B);' % off,
                  'cf = (v >> n) & 1u;']
            if mn == 'bts':
                st.append('wr8(B, (uint8_t)(v | (1u << n)));')
            elif mn == 'btr':
                st.append('wr8(B, (uint8_t)(v & ~(1u << n)));')
            elif mn == 'btc':
                st.append('wr8(B, (uint8_t)(v ^ (1u << n)));')
            st.append('}')
            return st
        bits = self.bits_of(d)
        st = ['{ unsigned n = (unsigned)(%s) & %du; %s v = %s; cf = (v >> n) & 1u;' % (self.rd(ins, s, 8 if s.type == X.X86_OP_IMM else bits), bits - 1, UT[bits], self.rd(ins, d))]
        if mn == 'bts':
            st.append(self.wr(ins, d, 'v | (1u << n)'))
        elif mn == 'btr':
            st.append(self.wr(ins, d, 'v & ~(1u << n)'))
        elif mn == 'btc':
            st.append(self.wr(ins, d, 'v ^ (1u << n)'))
        st.append('}')
        return st

    def string(self, ins, mn, prefix):
        kind, w = mn[:4], {'b': 8, 'w': 16, 'd': 32}[mn[4]]
        T = UT[w]
        n = w // 8
        acc = {8: '(uint8_t)eax', 16: '(uint16_t)eax', 32: 'eax'}[w]
        acc_w = {8: 'eax = (eax & 0xFFFFFF00u) | (uint8_t)v;', 16: 'eax = (eax & 0xFFFF0000u) | (uint16_t)v;', 32: 'eax = v;'}[w]
        step = '(df ? (uint32_t)-%d : %du)' % (n, n)
        if kind == 'movs':
            body = 'wr%d(edi, rd%d(esi)); esi += %s; edi += %s;' % (w, w, step, step)
        elif kind == 'stos':
            body = 'wr%d(edi, (%s)%s); edi += %s;' % (w, T, acc, step)
        elif kind == 'lods':
            body = '{ %s v = rd%d(esi); %s } esi += %s;' % (T, w, acc_w, step)
        elif kind == 'scas':
            body = '{ %s a = %s, b = rd%d(edi), r = (%s)(a - b); %s } edi += %s;' % (
                T, acc, w, T, ' '.join(self.flags_sub('a', 'b', 'r', w)), step)
        else:  # cmps: [esi] - [edi]
            body = '{ %s a = rd%d(esi), b = rd%d(edi), r = (%s)(a - b); %s } esi += %s; edi += %s;' % (
                T, w, w, T, ' '.join(self.flags_sub('a', 'b', 'r', w)), step, step)
        if prefix is None:
            return [body]
        if prefix == 'rep' and kind in ('movs', 'stos', 'lods'):
            return ['while (ecx) { %s ecx--; }' % body]
        if prefix in ('rep', 'repe', 'repz') and kind in ('scas', 'cmps'):
            return ['while (ecx) { %s ecx--; if (!zf) break; }' % body]
        if prefix in ('repne', 'repnz') and kind in ('scas', 'cmps'):
            return ['while (ecx) { %s ecx--; if (zf) break; }' % body]
        raise Unsupported('%s %s' % (prefix, mn))

    # ------------------------------------------------------------------ SSE / SSE2

    def xmm(self, ins, op):
        """The C lvalue of an XMM (X(n)) or MMX (MM(n)) register operand, or None. MMX registers
        are modelled apart from the x87 stack they alias (code that uses them runs emms before
        any x87), as X128s whose upper half stays zero: the lane-wise helpers then serve both."""
        if op.type != X.X86_OP_REG:
            return None
        name = ins.reg_name(op.reg)
        if name.startswith('xmm'):
            return 'X(%d)' % int(name[3:])
        if name.startswith('mm'):
            return 'MM(%d)' % int(name[2:])
        return None

    def xsrc(self, ins, op):
        """Statements loading an SSE source operand into the temporary S (a copy: helpers never
        see their destination and source alias)."""
        r = self.xmm(ins, op)
        if r is not None:
            return 'X128 S = %s;' % r
        if op.type == X.X86_OP_MEM:
            ld = {16: 'ld_x128', 8: 'ld_x64', 4: 'ld_x32'}.get(op.size)
            if ld is None:
                raise Unsupported('sse operand size %d' % op.size)
            return 'X128 S = %s(A);' % ld
        raise Unsupported('sse source')

    SSE_BIN = {
        # packed and scalar float arithmetic
        **{m + t: 'x_' + m + t for m in ('add', 'sub', 'mul', 'div', 'min', 'max') for t in ('ps', 'ss', 'pd', 'sd')},
        **{m + t: 'x_' + m + t for m in ('sqrt',) for t in ('ps', 'ss', 'pd', 'sd')},
        'rcpps': 'x_rcpps', 'rcpss': 'x_rcpss', 'rsqrtps': 'x_rsqrtps', 'rsqrtss': 'x_rsqrtss',
        # bitwise
        'andps': 'x_and', 'andpd': 'x_and', 'pand': 'x_and', 'andnps': 'x_andn', 'andnpd': 'x_andn', 'pandn': 'x_andn',
        'orps': 'x_or', 'orpd': 'x_or', 'por': 'x_or', 'xorps': 'x_xor', 'xorpd': 'x_xor', 'pxor': 'x_xor',
        # unpacks
        'unpcklps': 'x_unpcklps', 'unpckhps': 'x_unpckhps', 'unpcklpd': 'x_unpcklpd', 'unpckhpd': 'x_unpckhpd',
        # conversions between XMM registers
        **{m: 'x_' + m for m in ('cvtps2pd', 'cvtpd2ps', 'cvtss2sd', 'cvtsd2ss', 'cvtdq2ps', 'cvtdq2pd',
                                 'cvttps2dq', 'cvttpd2dq', 'cvtps2dq', 'cvtpd2dq')},
        # SSE2 integer
        **{m: 'x_' + m for m in (
            'paddb', 'paddw', 'paddd', 'paddq', 'psubb', 'psubw', 'psubd', 'psubq', 'pmullw', 'pmulhw', 'pmulhuw',
            'pavgb', 'pavgw', 'pminub', 'pmaxub', 'pminsw', 'pmaxsw', 'pcmpeqb', 'pcmpeqw', 'pcmpeqd', 'pcmpgtb',
            'pcmpgtw', 'pcmpgtd', 'paddsb', 'paddsw', 'paddusb', 'paddusw', 'psubsb', 'psubsw', 'psubusb', 'psubusw',
            'pmuludq', 'pmaddwd', 'psadbw', 'packsswb', 'packuswb', 'packssdw', 'punpcklbw', 'punpckhbw', 'punpcklwd',
            'punpckhwd', 'punpckldq', 'punpckhdq', 'punpcklqdq', 'punpckhqdq')},
    }
    SSE_IMM = {'shufps': 'x_shufps', 'shufpd': 'x_shufpd', 'pshufd': 'x_pshufd', 'pshuflw': 'x_pshuflw',
               'pshufhw': 'x_pshufhw', 'cmpps': 'x_cmpps', 'cmpss': 'x_cmpss', 'cmppd': 'x_cmppd', 'cmpsd': 'x_cmpsd'}
    SSE_CMP = ('eq', 'lt', 'le', 'unord', 'neq', 'nlt', 'nle', 'ord')
    SSE_MOV128 = ('movaps', 'movups', 'movapd', 'movupd', 'movdqa', 'movdqu', 'lddqu', 'movntps', 'movntpd', 'movntdq')
    SSE_GPR_ONLY = ('cvttss2si', 'cvtss2si', 'cvttsd2si', 'cvtsd2si')
    SSE_NOP = ('prefetchnta', 'prefetcht0', 'prefetcht1', 'prefetcht2', 'prefetch', 'prefetchw',
               'sfence', 'lfence', 'mfence', 'emms')

    def sse(self, ins, mn, ops):
        """C statements for an SSE/SSE2 instruction, or None when it is not one."""
        if mn in self.SSE_NOP:
            return []
        if mn in ('ldmxcsr', 'stmxcsr'):
            pro = self.mem_prologue(ins)
            return pro + (['g->mxcsr = rd32(A); sse_mxcsr_apply(g->mxcsr);'] if mn == 'ldmxcsr' else ['wr32(A, g->mxcsr);'])
        xr = [self.xmm(ins, o) for o in ops]
        if not any(r is not None for r in xr) and mn not in self.SSE_GPR_ONLY:
            return None
        pro = self.mem_prologue(ins)
        d = ops[0]
        dx = xr[0]

        # CMPccPS pseudo-mnemonics (capstone spells the predicate out)
        pred = None
        for k, c in enumerate(self.SSE_CMP):
            for t in ('ps', 'ss', 'pd', 'sd'):
                if mn == 'cmp' + c + t:
                    pred, mn = k, 'cmp' + t
        if mn in self.SSE_BIN and dx is not None and len(ops) == 2:
            mx = ', g->mxcsr' if mn in ('cvtps2dq', 'cvtpd2dq') else ''  # rounding follows MXCSR.RC
            return pro + ['{ ' + self.xsrc(ins, ops[1]), '%s(&%s, &S%s); }' % (self.SSE_BIN[mn], dx, mx)]
        if mn in self.SSE_IMM and dx is not None:
            imm = pred if pred is not None else ops[2].imm & 0xFF
            return pro + ['{ ' + self.xsrc(ins, ops[1]), '%s(&%s, &S, %du); }' % (self.SSE_IMM[mn], dx, imm)]

        if mn in self.SSE_MOV128:
            if d.type == X.X86_OP_MEM:
                return pro + ['st_x128(A, &%s);' % xr[1]]
            return pro + ['{ ' + self.xsrc(ins, ops[1]), '%s = S; }' % dx]
        if mn in ('movss', 'movsd'):
            w = 32 if mn == 'movss' else 64
            if d.type == X.X86_OP_MEM:
                return pro + ['st_x%d(A, &%s);' % (w, xr[1])]
            if ops[1].type == X.X86_OP_MEM:
                return pro + ['%s = ld_x%d(A);' % (dx, w)]
            return ['{ X128 S = %s; x_movlo%d(&%s, &S); }' % (xr[1], w, dx)]
        if mn in ('movlps', 'movlpd', 'movhps', 'movhpd'):
            hi = mn.startswith('movh')
            if d.type == X.X86_OP_MEM:
                return pro + ['st_x64%s(A, &%s);' % ('hi' if hi else '', xr[1])]
            return pro + ['{ X128 S = ld_x64(A); x_mov%s64(&%s, &S); }' % ('hi' if hi else 'lo', dx)]
        if mn in ('movlhps', 'movhlps'):
            return ['{ X128 S = %s; %s(&%s, &S); }' % (xr[1], 'x_movhi64' if mn == 'movlhps' else 'x_movhl', dx)]
        if mn == 'movd':
            if dx is not None:
                return pro + ['%s = x_from32(%s);' % (dx, self.rd(ins, ops[1], 32))]
            return pro + [self.wr(ins, d, '%s.u32[0]' % xr[1])]
        if mn == 'movq':
            if d.type == X.X86_OP_MEM:
                return pro + ['st_x64(A, &%s);' % xr[1]]
            if dx is not None:
                return pro + ['{ ' + self.xsrc(ins, ops[1]), 'x_movq(&%s, &S); }' % dx]
        if mn in ('cvtsi2ss', 'cvtsi2sd'):
            lane = 'f[0] = (float)' if mn == 'cvtsi2ss' else 'd[0] = (double)'
            return pro + ['%s.%s(int32_t)%s;' % (dx, lane, self.rd(ins, ops[1], 32))]
        if mn in ('cvttss2si', 'cvtss2si', 'cvttsd2si', 'cvtsd2si'):
            dbl = mn.endswith('sd2si')
            r = self.xmm(ins, ops[1])
            src = ('%s.%s' % (r, 'd[0]' if dbl else 'f[0]')) if r is not None else ('rdf64(A)' if dbl else 'rdf32(A)')
            conv = 'cvtt_f2i(%s)' % src if mn.startswith('cvtt') else 'cvt_f2i(g->mxcsr, %s)' % src
            return pro + [self.wr(ins, d, '(uint32_t)' + conv)]
        if mn in ('comiss', 'ucomiss', 'comisd', 'ucomisd'):
            dbl = mn.endswith('sd')
            lane = 'd[0]' if dbl else 'f[0]'
            r = xr[1]
            src = '%s.%s' % (r, lane) if r is not None else ('rdf64(A)' if dbl else 'rdf32(A)')
            return pro + ['SSE_COMI(%s.%s, %s);' % (dx, lane, src)]
        if mn in ('movmskps', 'movmskpd', 'pmovmskb'):
            return [self.wr(ins, d, 'x_%s(&%s)' % (mn, xr[1]))]
        if mn in ('psllw', 'pslld', 'psllq', 'psrlw', 'psrld', 'psrlq', 'psraw', 'psrad'):
            if ops[1].type == X.X86_OP_IMM:
                return ['x_%s(&%s, %du);' % (mn, dx, ops[1].imm & 0xFF)]
            return pro + ['{ ' + self.xsrc(ins, ops[1]), 'x_%s(&%s, S.u64[0]); }' % (mn, dx)]
        if mn in ('pslldq', 'psrldq'):
            return ['x_%s(&%s, %du);' % (mn, dx, ops[1].imm & 0xFF)]
        if mn == 'pextrw':
            return [self.wr(ins, d, '%s.u16[%d]' % (xr[1], ops[2].imm & 7))]
        if mn == 'pinsrw':
            return pro + ['%s.u16[%d] = (uint16_t)%s;' % (dx, ops[2].imm & 7, self.rd(ins, ops[1], 16 if ops[1].type == X.X86_OP_MEM else 32))]
        if mn == 'pshufw':
            return pro + ['{ ' + self.xsrc(ins, ops[1]), 'x_pshuflw(&%s, &S, %du); %s.u64[1] = 0; }' % (dx, ops[2].imm & 0xFF, dx)]
        if mn == 'cvtpi2ps':
            return pro + ['{ ' + self.xsrc(ins, ops[1]), '%s.f[0] = (float)S.s32[0]; %s.f[1] = (float)S.s32[1]; }' % (dx, dx)]
        if mn in ('cvttps2pi', 'cvtps2pi'):
            conv = 'cvtt_f2i(S.f[%d])' if mn == 'cvttps2pi' else 'cvt_f2i(g->mxcsr, S.f[%d])'
            return pro + ['{ ' + self.xsrc(ins, ops[1]), '%s.s32[0] = %s; %s.s32[1] = %s; %s.u64[1] = 0; }'
                          % (dx, conv % 0, dx, conv % 1, dx)]
        raise Unsupported(mn)

    # ------------------------------------------------------------------ x87

    @staticmethod
    def x87_opcode(ins):
        b = bytes(ins.bytes)
        i = 0
        while i < len(b) and b[i] in (0x9B, 0x66, 0x67, 0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0xF0, 0xF2, 0xF3):
            i += 1
        if i < len(b) - 1 and 0xD8 <= b[i] <= 0xDF:
            return b[i], b[i + 1]
        return None

    def x87(self, ins, opc):
        op, modrm = opc
        mod, reg, rm = modrm >> 6, (modrm >> 3) & 7, modrm & 7
        a = hexu(ins.address)
        memop = next((o for o in ins.operands if o.type == X.X86_OP_MEM), None)
        A = ['uint32_t A = %s;' % self.addr(ins, memop)] if memop is not None else []
        ARITH = {0: '{d} = fr(g, {d} + {s});', 1: '{d} = fr(g, {d} * {s});', 4: '{d} = fr(g, {d} - {s});',
                 5: '{d} = fr(g, {s} - {d});', 6: '{d} = fr(g, {d} / {s});', 7: '{d} = fr(g, {s} / {d});'}

        if mod != 3:  # memory forms
            if op in (0xD8, 0xDC, 0xDA, 0xDE):
                src = {0xD8: 'rdf32(A)', 0xDC: 'rdf64(A)', 0xDA: '(double)(int32_t)rd32(A)', 0xDE: '(double)(int16_t)rd16(A)'}[op]
                if reg in (2, 3):
                    return A + ['fcom(g, ST(0), %s);' % src] + (['fpop(g);'] if reg == 3 else [])
                return A + [ARITH[reg].format(d='ST(0)', s=src)]
            if op == 0xD9:
                return A + self.pick(ins, reg, {0: ['fpush(g, rdf32(A));'], 2: ['wrf32(A, (float)ST(0));'], 3: ['wrf32(A, (float)fpop(g));'],
                            4: ['fenv_load(g, A);'], 5: ['g->fcw = rd16(A);'],
                            6: ['fenv_store(g, A); g->fcw |= 0x3Fu;'], 7: ['wr16(A, g->fcw);']})
            if op == 0xDB:
                return A + self.pick(ins, reg, {0: ['fpush(g, (double)(int32_t)rd32(A));'], 1: ['wr32(A, (uint32_t)(int32_t)trunc(fpop(g)));'],
                            2: ['wr32(A, (uint32_t)fist32(g, ST(0)));'], 3: ['wr32(A, (uint32_t)fist32(g, fpop(g)));'],
                            5: ['fpush(g, rdf80(A));'], 7: ['wrf80(A, fpop(g));']})
            if op == 0xDD:
                return A + self.pick(ins, reg, {0: ['fpush(g, rdf64(A));'], 2: ['wrf64(A, ST(0));'], 3: ['wrf64(A, fpop(g));'],
                            4: ['frstor(g, A);'], 6: ['fsave(g, A);'], 7: ['wr16(A, fstsw(g));']})
            if op == 0xDF:
                return A + self.pick(ins, reg, {0: ['fpush(g, (double)(int16_t)rd16(A));'], 2: ['wr16(A, (uint16_t)fist16(g, ST(0)));'],
                            3: ['wr16(A, (uint16_t)fist16(g, fpop(g)));'], 5: ['fpush(g, (double)(int64_t)rd64(A));'],
                            7: ['wr64(A, (uint64_t)fist64(g, fpop(g)));']})
            return self.x87_bad(ins)

        i = rm  # register forms: ST(i)
        if op == 0xD8:
            if reg in (2, 3):
                return ['fcom(g, ST(0), ST(%d));' % i] + (['fpop(g);'] if reg == 3 else [])
            return [ARITH[reg].format(d='ST(0)', s='ST(%d)' % i)]
        if op == 0xDC:
            # DC: ST(i) = ST(i) op ST(0); E0+i FSUBR (ST(i) = ST(0) - ST(i)), E8+i FSUB, F0+i FDIVR, F8+i FDIV
            m = {0: '{d} = fr(g, {d} + {s});', 1: '{d} = fr(g, {d} * {s});', 4: '{d} = fr(g, {s} - {d});',
                 5: '{d} = fr(g, {d} - {s});', 6: '{d} = fr(g, {s} / {d});', 7: '{d} = fr(g, {d} / {s});'}
            if reg in m:
                return [m[reg].format(d='ST(%d)' % i, s='ST(0)')]
            return self.x87_bad(ins)
        if op == 0xDE:
            if modrm == 0xD9:
                return ['fcom(g, ST(0), ST(1)); fpop(g); fpop(g);']
            m = {0: '{d} = fr(g, {d} + {s});', 1: '{d} = fr(g, {d} * {s});', 4: '{d} = fr(g, {s} - {d});',
                 5: '{d} = fr(g, {d} - {s});', 6: '{d} = fr(g, {s} / {d});', 7: '{d} = fr(g, {d} / {s});'}
            if reg in m:
                return [m[reg].format(d='ST(%d)' % i, s='ST(0)'), 'fpop(g);']
            return self.x87_bad(ins)
        if op == 0xD9:
            if reg == 0:
                return ['{ double v = ST(%d); fpush(g, v); }' % i]
            if reg == 1:
                return ['{ double t = ST(0); ST(0) = ST(%d); ST(%d) = t; }' % (i, i)]
            simple = {
                0xD0: [], 0xE0: ['ST(0) = -ST(0);'], 0xE1: ['ST(0) = fabs(ST(0));'], 0xE4: ['fcom(g, ST(0), 0.0);'],
                0xE5: ['fxam(g);'], 0xE8: ['fpush(g, 1.0);'], 0xE9: ['fpush(g, 3.3219280948873623);'],
                0xEA: ['fpush(g, 1.4426950408889634);'], 0xEB: ['fpush(g, 3.141592653589793);'],
                0xEC: ['fpush(g, 0.30102999566398120);'], 0xED: ['fpush(g, 0.69314718055994531);'], 0xEE: ['fpush(g, 0.0);'],
                0xF0: ['ST(0) = fr(g, exp2(ST(0)) - 1.0);'],
                0xF1: ['{ double x = fpop(g); ST(0) = fr(g, ST(0) * log2(x)); }'],
                0xF2: ['ST(0) = fr(g, tan(ST(0))); fpush(g, 1.0); g->c2 = 0;'],
                0xF3: ['{ double x = fpop(g); ST(0) = fr(g, atan2(ST(0), x)); }'],
                0xF5: ['ST(0) = fr(g, remainder(ST(0), ST(1))); g->c2 = 0;'],
                0xF6: ['g->top = (g->top - 1) & 7u;'], 0xF7: ['g->top = (g->top + 1) & 7u;'],
                0xF8: ['ST(0) = fr(g, fmod(ST(0), ST(1))); g->c2 = 0;'],
                0xF9: ['{ double x = fpop(g); ST(0) = fr(g, ST(0) * log2(x + 1.0)); }'],
                0xFA: ['ST(0) = fr(g, sqrt(ST(0)));'],
                0xFB: ['{ double x = ST(0); ST(0) = fr(g, sin(x)); fpush(g, fr(g, cos(x))); g->c2 = 0; }'],
                0xFC: ['ST(0) = frnd(g, ST(0));'],
                0xFD: ['ST(0) = fr(g, ldexp(ST(0), (int)trunc(ST(1))));'],
                0xFE: ['ST(0) = fr(g, sin(ST(0))); g->c2 = 0;'], 0xFF: ['ST(0) = fr(g, cos(ST(0))); g->c2 = 0;'],
            }
            if modrm in simple:
                return simple[modrm]
            return self.x87_bad(ins)
        if op == 0xDA and modrm == 0xE9:
            return ['fcom(g, ST(0), ST(1)); fpop(g); fpop(g);']
        if op == 0xDB:
            if modrm == 0xE2:
                return ['g->c0 = g->c1 = g->c2 = g->c3 = 0;']
            if modrm == 0xE3:
                return ['g->fcw = 0x037F; g->top = 0; g->c0 = g->c1 = g->c2 = g->c3 = 0;']
            if reg in (5, 6):
                return self.fcomi(i, False)
            return self.x87_bad(ins)
        if op == 0xDD:
            if reg == 0:
                return []  # FFREE: tags are not modelled
            if reg == 2:
                return ['ST(%d) = ST(0);' % i]
            if reg == 3:
                return ['ST(%d) = ST(0); fpop(g);' % i]
            if reg in (4, 5):
                return ['fcom(g, ST(0), ST(%d));' % i] + (['fpop(g);'] if reg == 5 else [])
            return self.x87_bad(ins)
        if op == 0xDF and modrm == 0xE0:
            return ['eax = (eax & 0xFFFF0000u) | fstsw(g);']
        if op == 0xDF and reg in (5, 6):
            return self.fcomi(i, True)
        return self.x87_bad(ins)

    def pick(self, ins, reg, table):
        r = table.get(reg)
        if r is None:
            self.x87_bad(ins)
        return r

    def fcomi(self, i, pop):
        """FCOMI/FUCOMI(P): compare ST(0) with ST(i) into ZF PF CF (unordered sets all three)."""
        return ['{ double a = ST(0), b = ST(%d); if (a != a || b != b) { zf = pf = cf = 1; } '
                'else { zf = a == b; pf = 0; cf = a < b; } of = sf = af = 0; }' % i] + (['fpop(g);'] if pop else [])

    def x87_bad(self, ins):
        raise Unsupported('x87 %s' % bytes(ins.bytes).hex())
