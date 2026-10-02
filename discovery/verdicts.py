# The manual verdicts, per module and per retail build. Imported by Discover2.py, Switches.py
# and BogusCheck.py (Jython), and read by tools/newbuild.py (CPython): keep it plain data, no
# Ghidra imports at module level. A build is recognised by the PE TimeDateStamp of the imported
# image, which the unpacker keeps from the retail DLL; the module by the image's name
# (FFXiMain.unpacked.dll -> 'ffximain', FFXi.unpacked.dll -> 'ffxi').
#
# The verdicts for a new build are the previous build's, carried over with an address map (a byte
# window around each old address, relocated dwords wildcarded, must match exactly once in the new
# .text: `tools/newbuild.py carry`) and then re-checked by the pipeline itself:
# Conflicts/BogusCheck/SwitchAudit report anything the carried-over lists miss. See the build's
# discovery notes (discovery/notes/).
BUILDS = {
    # FFXiMain.dll
    0x6a7297f5: '2026-08-22',
    0x6a995428: '2026-09-03',
    0x694e5965: '2025-12-26',
    # FFXi.dll
    0x6a7297e3: '2026-08-22',
    0x6a995417: '2026-09-03',
    0x694e5955: '2025-12-26',
}

# NOT_CODE: false function starts (data records decoded as code, or aligned starts inside a real
# instruction). REDECODE: the real decode points those false starts were hiding.
# SWITCHES: switches whose case count cannot be read from a guard in their own function.
VERDICTS = {
    ('ffximain', '2026-08-22'): {
        'NOT_CODE': [0x1030df80, 0x1030ef80, 0x1030eff0,  # 16-byte records {ptr, int, flags} in .text
                     0x10310f40,                          # inside an 11-byte `mov eax,[g]; jmp [eax+off]` stub
                     0x1030fdd0,                          # inside the 6-byte import-thunk run
                     0x1015bef0, 0x101aec18],             # inside `mov [edi+0xccc],esi` etc.
        'REDECODE': [0x1015beed, 0x101aec17, 0x10310f38],
        'SWITCHES': {
            0x1022222d: 9,   # jmp [ecx*4+0x1022243c], ecx = byte [ebx+0x10222460]; the index table starts at entry 9
            0x102a72e8: 4,   # jmp [ecx*4+0x102a769c], ecx = arg [esp+0x38]; entry 4 is 0x90909090 padding
            # Signed two-sided guards (cmp r,-N / jb ; cmp r,N / ja ; add r,N) that both Ghidra and the
            # one-cmp reader under-count.
            0x10006412: 21,  # ecx in -10..10; table ends at the next table 0x100077dc
            0x10006ff7: 11,  # edx in -5..5; entry 11 is padding
            0x102bad09: 6,   # cmp edx,5 / ja; Ghidra attached 1 case
        },
    },
    # The same sites in the 2026-09-03 build (0x1015bee0: the instruction is now `mov [edi+0x14cc],esi`,
    # still with a 0xcc byte right before the aligned address).
    # Three false starts are new in this build (Conflicts.py): 0x100657d0 inside `mov eax,0x10353b90`
    # at 0x100657ce (its 0x90 read as padding); 0x100f46b0, a 4-entry switch table after a nop; and
    # 0x1015d7e0, a jump table into the function before it, right after its ret.
    ('ffximain', '2026-09-03'): {
        'NOT_CODE': [0x1030df40, 0x1030ef40, 0x1030efb0, 0x10310f00, 0x1030fd90, 0x1015bee0, 0x101aec08,
                     0x100657d0, 0x100f46b0, 0x1015d7e0],
        'REDECODE': [0x1015bedd, 0x101aec07, 0x10310ef8, 0x100657ce],
        'SWITCHES': {0x1022221d: 9, 0x102a72d8: 4, 0x10006412: 21, 0x10006ff7: 11, 0x102bacf9: 6},
    },
    # FFXi.dll carries 672 11-byte stubs `mov eax,[0x10018b40]; jmp [eax+off]` (0x100057a0-0x10007480),
    # most never referenced. A displacement byte 0x90 (`jmp [eax+0xe90]`) read as nop padding
    # made the aligned-start heuristic start a function mid-stub. Its .text is byte-identical in
    # all three builds.
    ('ffxi', '2026-08-22'): {'NOT_CODE': [0x100067d0], 'REDECODE': [0x100067c8], 'SWITCHES': {}},
    ('ffxi', '2026-09-03'): {'NOT_CODE': [0x100067d0], 'REDECODE': [0x100067c8], 'SWITCHES': {}},
    # 2025-12-26, an older client than the two above: carried over from 2026-09-03 (the 2026-08-22
    # carry lands on the same addresses). 0x1015ac10 inside `mov [edi+0xccc],esi` at 0x1015ac0d,
    # as in 2026-08-22; 0x1015c510 a 4-entry jump table into the function before it, right after
    # its ret; 0x100f42a0 the 4-entry switch table after a nop. 2026-09-03's 0x100657d0 pair is
    # dropped: here the instruction is `mov eax,0x10351ba0` at 0x100655de, with no 0x90 byte.
    # New in this build (Conflicts.py): 0x10007670, the 21-case table of `jmp [ecx*4+0x10007670]`
    # at 0x100063fc right after a ret, taken as a function (its first byte 0xc0 decodes as shl;
    # in 2026-09-03 it was 0xd6, salc, which the after-ret check rejects).
    ('ffximain', '2025-12-26'): {
        'NOT_CODE': [0x1030c500, 0x1030d500, 0x1030d570, 0x1030f4c0, 0x1030e350, 0x1015ac10, 0x101ad368,
                     0x100f42a0, 0x1015c510, 0x10007670],
        'REDECODE': [0x1015ac0d, 0x101ad367, 0x1030f4b8],
        'SWITCHES': {0x100063fc: 21, 0x10006fe1: 11, 0x102208dd: 9, 0x102a5888: 4, 0x102b92a9: 6},
    },
    ('ffxi', '2025-12-26'): {'NOT_CODE': [0x100067d0], 'REDECODE': [0x100067c8], 'SWITCHES': {}},
}


def build_of(prog):
    mem = prog.getMemory()
    base = prog.getImageBase()
    lfanew = mem.getInt(base.add(0x3c))
    ts = mem.getInt(base.add(lfanew + 8)) & 0xFFFFFFFF
    if ts not in BUILDS:
        raise Exception('unknown build: PE timestamp 0x%08x; add it to discovery/verdicts.py (tools/newbuild.py carry)' % ts)
    return BUILDS[ts]


def verdicts(prog):
    module = prog.getName().split('.')[0].lower()
    key = (module, build_of(prog))
    if key not in VERDICTS:
        raise Exception('no manual verdicts for %s %s; add them to discovery/verdicts.py' % key)
    return VERDICTS[key]
