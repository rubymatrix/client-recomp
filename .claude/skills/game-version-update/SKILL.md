---
name: game-version-update
description: End-to-end support for a new FINAL FANTASY XI client version, from install folder to recompiled builds on every platform. Use when the user hands over a new game version or install ("here's my new game version", "the game patched", "new FFXiMain.dll", a private-server install), when prepare.py says a DLL "is build <sha>, which meta/builds.json does not know", or when asked to add or support a new build.
---

# New game version, end to end

The user points at a `FINAL FANTASY XI` folder (the viewer folder may sit beside it). Take it from
there to a committed-ready build entry and translated, tested binaries on every platform target.
This skill runs two others in order:

1. **game-version-analyze**: identify, unpack, carry addresses over, and run the discovery pass.
   The result is `meta/<Module>.<label>.meta.json` and a `meta/builds.json` entry.
2. **game-version-build**: prepare, then translate, build and test each platform target.

## Before starting

- Get the install folder. If the user gave a zip or an installer instead, ask where the unpacked
  `FINAL FANTASY XI` folder is; never write into the install.
- **No Square Enix bytes are ever committed** (README, *Rules*). Images, Ghidra projects and
  generated C stay in `generated/`, which is gitignored. Commit only `meta/`, `discovery/verdicts.py`,
  `discovery/notes/`, README and tooling.
- Note the working tree state (`git status`). Other work may be uncommitted. Touch only the files
  this flow owns, and never stage unrelated changes.

## Flow

```
python tools/newbuild.py identify --game "<FINAL FANTASY XI>"
```

- **Already known** (`known_build` set): nothing to analyze. Run game-version-build for that
  build and stop.
- **New**: run game-version-analyze, then game-version-build.

Report progress to the user at each phase boundary: the label and version, what carried over,
the gate result, and each platform's result.

## Finish

1. Write `discovery/notes/<Module>.<label>.discovery.txt` for each module that went through the
   discovery pass. Follow `discovery/notes/FFXiMain.2026-09-03.discovery.txt`: the retail files and
   hashes, what changed from the previous build, what carried over and what was mapped by hand,
   new verdicts with their evidence, the results table, and verification.
2. README: add the build to the table under *Rules* (label, shortened SHA-256, version, where the
   install came from).
3. Summarize for the user:
   - the build label and version string, and whether the version was read from `patch.ver` or
     inferred. An inferred version may be refused by a server's lobby check, so say so.
   - function, switch and tail-jump counts against the previous build.
   - every manual verdict added.
   - each platform target: PASS, FAIL, or not run (and why, e.g. "macOS needs the Mac").
   - what's left: an in-game test (sign in, zone in, fight, zone again) and the commit.
4. Offer to commit `meta/`, `discovery/`, and README together as one commit. Commit only when the
   user says so.
