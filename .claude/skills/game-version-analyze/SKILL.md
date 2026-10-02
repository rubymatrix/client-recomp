---
name: game-version-analyze
description: Unpack and analyze a new FINAL FANTASY XI client build to produce its recompiler metadata. Covers identifying the build, unpacking the packed `.text`, carrying addresses over from the previous build, the Ghidra discovery pass, and triaging the discovery gate. Use for "analyze this game version", "run the discovery pass", "make metadata for build X", or as phase 1 of game-version-update.
---

# Analyze a new game version

**Input:** a `FINAL FANTASY XI` folder.
**Output:**
- `meta/FFXiMain.<label>.meta.json` and `meta/FFXi.<label>.meta.json`
- the build's entry in `meta/builds.json`
- its entry in `discovery/verdicts.py`

All tools run from the repo root. `generated/` holds everything derived from the game and is never
committed.

Needs Python with `pefile` and `capstone`, plus Ghidra (11.3.2 tested) and a JDK (21). If
`tools/discover.py` can't find Ghidra, it looks in `GHIDRA_INSTALL_DIR` and in
`C:\Dev\Ghidra\ghidra_*_PUBLIC`.

## 1. Identify

```
python tools/newbuild.py identify --game "<FINAL FANTASY XI>"
```

- `label`: the UTC date of `FFXiMain.dll`'s PE timestamp. Labels must be unique; if one
  collides with a different build, stop and ask the user.
- `version`: decrypted from `patch.ver` when the install has one. Otherwise it is inferred as
  `30` + YYMMDD. Tell the user when it is inferred: a server whose lobby expects another string
  will refuse the client, and the fix is just to edit `version` in `meta/builds.json`.
- If `known_build` is set, this build is already supported. Skip to game-version-build.

## 2. Unpack

```
python tools/newbuild.py unpack --game "<FINAL FANTASY XI>"
```

This writes `generated/images/<label>/` and reports whether each module's `.text` is identical to
a known build's. The comparison only covers builds whose images are in `generated/images/`
(`prepare.py` keeps one per build it prepares). If the previous build's images are missing, unpack
its install too, or ask the user where it is.

If `unpack.py` refuses the stub ("not the packer stub", "unexpected prologue"), the packer changed.
Stop and report it; that is a tooling change, not a routine update.

## 3. Carry the previous build over

Pick the newest known build (the last in `meta/builds.json`) as `<old>`.

```
python tools/newbuild.py carry --from <old> --to <label>          # dry run: read the report
python tools/newbuild.py carry --from <old> --to <label> --write  # builds.json + verdicts.py
```

The report maps these onto the new image:
- `chars_ptr`: a global, found by voting over the code sites that reference it.
- `present_site`: the return address after `call [ecx+0x3c]`, IDirect3DDevice8::Present.
- the 15 CRT functions that difftest compares.
- the manual verdicts (NOT_CODE, REDECODE, SWITCHES).

Deltas are usually small and uniform (for example 0, -0x10, -0x40). A mapping far off from its
neighbours' delta is suspect, so check it with `dis`.

**Unmapped addresses** come with a hint (`neighbours moved by -0x10 -> try 0x...`). Verify each
one by disassembling both builds:

```
python tools/newbuild.py dis --label <old>   --at <old addr> --before 24 --count 4
python tools/newbuild.py dis --label <label> --at <hinted>  --before 24 --count 4
```

Accept the hint only when the instruction sequence is the same, allowing for changed struct
displacements or immediates. Then:
- a verdict goes into the new `VERDICTS` entry in `discovery/verdicts.py`, replacing its
  `# TODO unmapped` line.
- an address or CRT entry goes into `meta/builds.json`, replacing the `UNMAPPED from ...` string.

Nothing may be left UNMAPPED. `build.py` would crash on it, and the discovery scripts would run
without the verdict.

**A module whose `.text` is identical** to `<old>`'s (common for `FFXi.dll`) skips discovery:

```
python tools/newbuild.py meta --from <old> --to <label> --module FFXi.dll
```

## 4. Discovery pass

For each module whose `.text` changed:

```
python tools/discover.py --label <label> --module FFXiMain.dll
```

Run it in the background. Import and auto-analysis take several minutes for `FFXiMain.dll`, then
there are about 12 headless steps of roughly 20–60 s each. Reports and logs go to
`generated/discovery/<label>/<Module>/`.

**The gate**: after two rounds, conflicts, suspect_functions and switch review must all be 0.
Otherwise it exits 2 without exporting. Triage each reported site and record a verdict with a
one-line evidence comment, as the existing entries have:

- **Conflicts** (`X falls into Y ... overlapping=...`): a false function start overlaps real code.
  `dis --at <false start>` shows it INSIDE a real instruction. Add the false start to `NOT_CODE`
  and the real instruction's start to `REDECODE`. Typical causes:
  - a `0x90` or `0xCC` byte inside an operand, read as padding by the aligned-start heuristic.
  - a jump or switch table after a `ret` or `nop`. That one is `NOT_CODE` only.
- **BogusCheck suspects** (`score=`, `zero_adds`, `rare`): data decoded as code. Confirm the bytes
  are records, tables or strings, not instructions, then add them to `NOT_CODE`.
- **SwitchAudit REVIEW** (`attached=N plausible_run=M`): read the guard before the
  `jmp [reg*4+table]`. A signed two-sided guard (`cmp r,-N / jb ; cmp r,N / ja ; add r,N`), or an
  index bounded by the caller, gets `SWITCHES[<jmp addr>] = <case count>`. Check that the last
  table entry is a real case, not padding or the next table.
- `Switches.py` lines marked UNBOUNDED or BAD_TABLE are expected for guard-less switches such as
  the CRT's `memcpy`/`memmove`. They are not in the gate.

Then rerun with `--reuse`, which keeps the analysed project and skips import. Repeat until the gate
passes. If a verdict is truly ambiguous, show the user the disassembly and ask. Don't guess: a wrong
NOT_CODE deletes real code from the translation.

**Sanity check the export** against the previous build's metadata. Function count, switches and
tail jumps should be within about 1% (for example 21,873 → 21,762, 943 → 943). Report any big
swing to the user before going on.

## 5. Hand off

State:
- the label and version (read from `patch.ver` or inferred)
- for each module: metadata carried over or discovered, with counts
- every verdict added, with its evidence

Then continue with game-version-build.
