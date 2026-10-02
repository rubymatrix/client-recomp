"""Static unpacker: rebuild a packed FFXI DLL's .text without running it.

The packer's stub (FFXiMain.dll build 2026-08-22: entry 0x10bb1a60), on DLL_PROCESS_ATTACH only:

  1. decompress(src=packed section start, src_len=<imm>, dst=.text, &dst_len=<imm>)
  2. apply a private .text relocation table (stored at the start of .reloc) with a delta that is
     hard-coded to 0, so at the preferred image base it changes nothing
  3. jmp to the real entry point (the CRT's _DllMainCRTStartup)

No integrity checks, no anti-debugging, no import tricks. The compression is a plain LZSS:

  flag byte, then 8 items, most significant flag bit first
    bit 1: literal byte
    bit 0: two bytes b0 b1; offset = ((b0 << 8) | b1) & 0xfff, length = (b0 >> 4) + 3,
           copy byte by byte from (out - offset) (overlap allowed); offset 0 ends the stream

The stub's immediates (src_len, dst_len, the original entry point) are parsed out of the stub
itself, so this works for any packed module whose stub has the same shape — it refuses otherwise.

Usage: python unpack.py <packed.dll> <out.dll> [--check <dumped.dll>]
The output is the input with .text filled in and marked as code, like unpacked/rebuild.py
produces from a LoadLibrary dump. With --check, .text is compared against a dump.
"""
import struct
import sys

import pefile


def lzss_decompress(src, expected_len):
    out = bytearray()
    i = 0
    n = len(src)
    while i < n:
        flags = src[i]
        i += 1
        for _ in range(8):
            if flags & 0x80:  # `shl bl,1 ; jae match`: carry set = literal
                out.append(src[i])
                i += 1
            else:
                b0, b1 = src[i], src[i + 1]
                offset = ((b0 << 8) | b1) & 0xFFF
                if offset == 0:
                    return bytes(out)
                i += 2
                length = (b0 >> 4) + 3
                start = len(out) - offset
                if start < 0:
                    raise ValueError('back-reference before start of output at src %#x' % i)
                for k in range(length):
                    out.append(out[start + k])
            flags = (flags << 1) & 0xFF
            if len(out) > expected_len + 0x1000:
                raise ValueError('output overran the expected .text size')
    raise ValueError('stream ended without an end marker')


def packed_section(pe):
    """The packed section: the one the entry point (the stub) is in."""
    ep = pe.OPTIONAL_HEADER.AddressOfEntryPoint
    return next(s for s in pe.sections if s.VirtualAddress <= ep < s.VirtualAddress + s.Misc_VirtualSize)


def parse_stub(pe):
    """Read src_len, dst_len and the original entry point out of the stub.

    Expected shape (addresses from FFXiMain 2026-08-22):
      cmp byte ptr [esp+8],1 ; jne <tail>          ; DLL_PROCESS_ATTACH only
      ... mov eax,<dst_len> ; ... push <src_len> ; ... call <decompress>
      ... <tail>: jmp <original entry>              ; the last instruction before <decompress>
    """
    import capstone
    ep = pe.OPTIONAL_HEADER.AddressOfEntryPoint
    code = pe.get_data(ep, 0x100)
    if code[:5] != b'\x80\x7c\x24\x08\x01':
        raise ValueError('entry point is not the packer stub (unexpected prologue %s)' % code[:5].hex())
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    insns = list(md.disasm_lite(code, ep))
    dst_len = src_len = decompress = None
    last_push = None
    for addr, size, mn, op in insns:
        if mn == 'mov' and op.startswith('eax, 0x') and dst_len is None:
            dst_len = int(op[5:], 16)
        elif mn == 'push' and op.startswith('0x'):
            last_push = int(op, 16)
        elif mn == 'call':
            decompress = int(op, 16)
            src_len = last_push
            break
    if None in (dst_len, src_len, decompress):
        raise ValueError('could not read dst_len/src_len/decompressor from the stub')
    oep = None
    for addr, size, mn, op in insns:
        if addr >= decompress:
            break
        if mn == 'jmp' and op.startswith('0x'):
            oep = int(op, 16)  # keep the last one before the decompressor
    if oep is None:
        raise ValueError('no jmp to the original entry point before the decompressor')
    return src_len, dst_len, oep


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    src_path, out_path = sys.argv[1], sys.argv[2]
    check = sys.argv[4] if len(sys.argv) > 4 and sys.argv[3] == '--check' else None

    pe = pefile.PE(src_path)
    text = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.text')
    packed = packed_section(pe)
    if text.SizeOfRawData != 0:
        raise SystemExit('.text already has raw data: not packed')
    src_len, dst_len, oep = parse_stub(pe)
    print('stub: src_len=%#x dst_len=%#x original_entry=%#x (image %#x)' % (
        src_len, dst_len, oep, pe.OPTIONAL_HEADER.ImageBase + oep))
    if dst_len != text.Misc_VirtualSize:
        raise SystemExit('dst_len %#x != .text virtual size %#x' % (dst_len, text.Misc_VirtualSize))

    code = lzss_decompress(packed.get_data()[:src_len], dst_len)
    if len(code) != dst_len:
        raise SystemExit('decompressed %#x bytes, expected %#x' % (len(code), dst_len))
    print('decompressed %d bytes' % len(code))

    if check:
        dump = pefile.PE(check, fast_load=True)
        dtext = next(s for s in dump.sections if s.Name.rstrip(b'\0') == b'.text')
        ref = dtext.get_data()[:dst_len]
        diffs = [o for o in range(dst_len) if code[o] != ref[o]]
        print('check against %s: %d differing bytes%s' % (
            check, len(diffs), (' (first at .text+%#x)' % diffs[0]) if diffs else ''))

    # Splice: .text gets raw data appended at the end of the file, marked as code.
    align = pe.OPTIONAL_HEADER.FileAlignment
    raw = bytearray(pe.__data__)
    pad = (-len(raw)) % align
    raw += b'\0' * pad
    text_ptr = len(raw)
    blob = code + b'\0' * ((-len(code)) % align)
    raw += blob
    pe2 = pefile.PE(data=bytes(raw))
    t2 = next(s for s in pe2.sections if s.Name.rstrip(b'\0') == b'.text')
    t2.PointerToRawData = text_ptr
    t2.SizeOfRawData = len(blob)
    t2.Characteristics = (t2.Characteristics & ~0x80) | 0x20  # CNT_UNINITIALIZED_DATA -> CNT_CODE
    pe2.OPTIONAL_HEADER.AddressOfEntryPoint = oep  # skip the stub: .text is already unpacked
    pe2.write(out_path)
    print('wrote %s' % out_path)


if __name__ == '__main__':
    main()
