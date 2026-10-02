---
name: game-version-build
description: Recompile and test a FINAL FANTASY XI build for every platform target (Windows x86, Windows x64, macOS arm64) and report pass or fail per target. Use after new metadata lands, when asked to "rebuild all targets", "recompile for every platform", "build the new version", or as phase 2 of game-version-update.
---

# Build every platform target

**Input:** a `FINAL FANTASY XI` folder whose build is in `meta/builds.json` with its metadata in
`meta/`.

The same translation (`recomp/recomp.py` → `generated/all`, plus `generated/ffxi` for `FFXi.dll`)
feeds every target. Only the compiler, runtime and graphics back end differ.

| target | host | driver | test | pass means |
| --- | --- | --- | --- | --- |
| x86 difftest | Windows, MSVC | `python tools/build.py difftest` | the translated CRT slice against the original | `PASS: <n> calls, 0 mismatches` |
| x86 stand-in DLL | Windows, MSVC | `python tools/build.py host` | `boot.exe` at the preferred base, then relocated | `translated <n> functions`, then `BOOT OK` twice |
| x64 boot | Windows, MSVC | `python tools/build.py boot64` | the portable runtime boots the game image | `BOOT64 OK` |
| x64 game host | Windows, MSVC + SDL3 + D3D12 | `python tools/build.py host64` | links `build\host64.exe` | `built build\host64.exe` |
| arm64 boot | macOS, clang | `python3 tools/build_posix.py boot64 --game <folder>` | as x64 boot | `BOOT64 OK` |
| arm64 game host | macOS, clang + SDL3 + Metal + mbedtls | `python3 tools/build_posix.py host64 --game <folder>` | links `build/host64` | `built build/host64` |

`gfxtest` (both drivers) needs no game and doesn't change with a game version. Run it only if
runtime or graphics code changed too.

## Steps

1. **Prepare** (all hosts). This identifies the build, unpacks it into `generated/`, and records it
   in `generated/build.json`:

   ```
   python tools/prepare.py --game "<FINAL FANTASY XI>"             # Windows
   python3 tools/build_posix.py prepare --game "<FINAL FANTASY XI>"   # macOS / Linux
   ```

   Expect `ok: ... (build <label>, ...)`. "does not know" means the metadata isn't in place: run
   game-version-analyze.

2. **Build the targets this machine can build**, in the order above. Each one is incremental.
   The first translation of a new build recompiles all ~22k functions, which takes several
   minutes, so run it in the background. Capture each target's output to
   `build/<target>_build.log` and check the pass line from the table. Don't infer success from a
   zero exit code alone.

   - Windows needs Visual Studio (found through `vswhere`) and, for `host64`, the SDL3 VC dev
     package at `C:\Dev\SDL3\SDL3-3.4.16`, or wherever `SDL3_DIR` points.
   - `boot64` also exports the game's registry keys (from a retail install on this machine) to
     `build/ffxi.reg`. On a machine without one that is expected to fail quietly.

3. **macOS arm64.** If this session runs on macOS, build the two POSIX targets (`brew install
   sdl3 pkg-config mbedtls`, `pip3 install capstone pefile`). On Windows it can't be built here.
   Don't mark it passed. Report it as pending and give the user the steps for the Mac:
   - the new build's `meta/` files and `builds.json` entry must be on the Mac, by commit and pull.
   - the install's `SquareEnix` folder (`FINAL FANTASY XI`, and the viewer folder beside it if
     there is one) copied to e.g. `~/SquareEnix`, where `build_posix.py` looks by default.
   - then `prepare`, `boot64` and `host64` from the table above.

   If the user has a way to reach the Mac (ssh, a remote session), ask before using it.

4. **Linux** (optional). `build_posix.py` works there with the null graphics back end. It's
   useful as a translation smoke test (`boot64`), not a playable target.

## Failures

- **difftest mismatches**: a CRT address in `builds.json` is wrong for this build, or the
  translator broke. Check the named function with
  `python tools/newbuild.py dis --label <label> --at <crt addr>`. It should be the start of the
  same routine as in the previous build.
- **`translated N` far from the metadata's function count**, or a BOOT failure: see
  `build/host.log` and `build/boot64.log`. `FATAL at <addr>: indirect call/jump to an address with
  no translation` means the metadata lacks a function at that address. Go back to
  game-version-analyze: find why discovery missed it, add the verdict or seed, and re-export.
- **Compile errors in `generated/`**: usually the translator meeting a new instruction form. Fix
  `recomp/x86c.py`, not the generated C.
- **`host64` link errors**: platform-layer code (for example new D3D12 work in the tree), not the
  game version. Report it separately.

## Report

Give one line per target: PASS with its numbers (calls and mismatches, functions translated),
FAIL with the first real error, or NOT RUN with the reason. Remind the user that the real test
is in-game: sign in, zone in, fight, and zone again.
