# Ashita and Windower addon support: design

Status: draft, 2026-09-30. Input: Vekien's VanaCore write-up (how CatsEyeXI's client core runs
Ashita v4 and Windower 4 addons and plugins without either present).

## 1. Goal and scope

**Goal: run unmodified Ashita v4 and Windower 4 Lua addons**, with their own libraries (`libs\`,
`res\`) unchanged, on every host64 target (macOS/Metal, Windows/D3D12, Xbox/UWP).

"Fully support" is defined operationally, not by feeling:

- **Corpus**: the official Ashita v4 addon repo, the Windower `Lua` repo (addons + libs), and a list
  of popular third-party addons we pin by commit.
- **Pass**: an addon loads, runs its events against a recorded session, and never hits an
  `unsupported` stub (section 13). The target is 100% of the corpus minus a published exception list
  (addons that patch game code, section 7.3, or need native DLLs).

Out of scope for v1:

- **Native plugins** (Ashita `.dll` plugins, Windower `Hook.dll` plugins). They are 32-bit x86 DLLs.
  Section 15 keeps the door open: we already recompile two PE modules (FFXiMain, FFXi) and have a
  thunk layer, so recompiling plugins is plausible later. Popular ones (FindAll, Minimap, Timers,
  Shorthand, cexidats) are better reimplemented as addons or host features first.
- Addons that inline-patch game code. They get a native option instead, one by one.

The README's "will never be supported" section is reversed by this work; rewrite it when phase 2
ships (it stays accurate for DLL injection and native plugins).

## 2. What our images give us for free

Every pattern in Vekien's doc matches our unpacked images, uniquely where it should. Our
2025-12-26 build **is CatsEyeXI's build**: the RVAs are identical to the doc's.

| Pattern | 2025-12-26 | 2026-09-03 |
|---|---|---|
| `ParseInput` (command line) | 0x10080bd0 | 0x10080dc0 |
| `WriteLine` (chat log writer) | 0x100765d0 | 0x100767c0 |
| chat manager global (input line) | 0x101b764e | 0x101b8eee |
| packet decrypt | 0x100dd410 | 0x100dd6c0 |
| packet encrypt | 0x100dd2e0 | 0x100dd590 |
| entity array | 0x10003a17 | 0x10003a17 |
| entity count | 0x10095798 | 0x10095988 |
| party | 0x102012c4 | 0x10202ba4 |
| player (2 hits, as Ashita expects) | 0x10146d4e | 0x1014735e |
| target | 0x100889e6 | 0x10088bd6 |
| inventory | 0x100eb145 | 0x100eb555 |
| auto-follow | 0x1001f063 | 0x1001f253 |
| cast bar | 0x1007b2f0 | 0x1007b4e0 |
| status icons | 0x10098d66 | 0x10098f56 |
| key items (2 hits) | 0x10097c60 | 0x10097e50 |
| job level fn | 0x100f0ba0 | 0x100f0fb0 |
| known spells (3rd hit) | 0x100efc90 | 0x100f00a0 |

(2026-08-22 not scanned yet.) Consequences:

- **Guest memory holds the original code bytes** (`pe_load` unpacks `.text` into the window), so
  `ashita.memory.find` and Ashita's own `ashita.pointers.ini` patterns work as they are, at run time.
- For host-side hooks we still record addresses at build time in `meta/builds.json`, like every
  other hook, because the recompiler needs them. `tools/newbuild.py` gets these patterns so a new
  build's hook addresses are found automatically, and the build fails if one is missing or ambiguous.

## 3. Architecture

```
            ┌──────────────────────── game thread (guest lock held) ────────────────────────┐
 SDL pump ─▶│ user32 pump ─▶ overlay input capture ─▶ WM_* to game wndproc                  │
            │ dinput reads ─▶ overlay blanking ─▶ game                                      │
            │                                                                               │
            │ f_ParseInput  ──wrap──▶ cmd router: host cmds → binds/aliases → addons → game │
            │ f_WriteLine   ──wrap──▶ text_in / incoming text  (modify / block)             │
            │ f_decrypt/enc ──wrap──▶ packet pipeline: packet_in/out, chunk events, state   │
            │ Present       ──hook──▶ addon frame: tasks, d3d_present / prerender, ImGui,   │
            │                         overlay draw (text, prims, ImGui) via gfx.h           │
            └───────────────────────────────────────────────────────────────────────────────┘
                                         │
                         host/addons/ (C)  +  one LuaJIT state per addon
                         xi.* core API  ◀── ashita.lua  /  windower.lua  (embedded Lua)
```

New code lives in `host/addons/` (C) and `host/addons/lua/` (embedded Lua). The overlay renderer
lives in `runtime/portable/overlay.*` because it sits on `gfx.h` next to `d3d8.c`.

## 4. Recompiler: function wrap hooks

Today's `--hooks` are observe/modify points before one instruction; they cannot skip code. Every
addon feature VanaCore builds on a detour needs **skip or replace**, so add a second hook kind.

- `recomp.py --wraps name=0xADDR,...`, fed from `builds[<label>].wraps` in `meta/builds.json`.
  The address must be a function entry in the metadata (build error otherwise).
- Emission: the translated body is renamed `f_X_body`, and `f_X` becomes

  ```c
  void f_10080dc0(Guest* g) {
      if (rt_wrap_parse_input) rt_wrap_parse_input(g);
      else f_10080dc0_body(g);
  }
  GuestFn rt_orig_parse_input = f_10080dc0_body;
  ```

  `rt_table` keeps pointing at `f_X`, so direct calls, indirect calls and vtable calls all hit the
  wrap. Cost when unset: one test per call.
- The host wrapper runs at function entry: `esp` points at the return address, arguments above it.
  - To run the original: call `rt_orig_<name>(g)` (stack untouched), then read or change `eax`.
  - To skip it: set `eax`, then `esp += 4` (cdecl) or `esp += 4 + 4*n` (stdcall). A helper
    `rt_return(g, value, argbytes)` does it.
  - To change arguments: rewrite the stack slots, or point them at a `gheap_alloc` buffer.
- `build.h` gets `FFXI_WRAP_<NAME>`, and host code guards on it, exactly like `FFXI_HOOK_*`.

About 40 lines in `x86c.py` / `recomp.py`, and it serves every hook below.

## 5. Game hooks

All run on the game thread with the guest lock held. Per-build addresses come from `builds.json`.

| Hook | Kind | Use |
|---|---|---|
| `parse_input` (`int __cdecl (const char*, int mode)`) | wrap | Command routing (section 9), Ashita `command` and `text_out`, Windower `outgoing text`, `/shutdown` notice |
| `write_line` (`void __cdecl (int mode, const char*)`) | wrap + called | Ashita `text_in`, Windower `incoming text` (both may rewrite text/mode or block); `add_to_chat` / `AddChatMessage` call it via `guest_call`, queued until the chat log global is non-null |
| `packet_decrypt` | wrap | After the original returns: incoming pipeline (section 10) |
| `packet_encrypt` | wrap | Before the original runs: outgoing pipeline |
| Present | existing `d3d8_set_present_hook` | Addon frame (section 11) |
| DirectInput reads / user32 pump | direct code in `dinput.c`, `user32.c` | Key/mouse events, capture, binds (section 12) |

Game data (entities, party, player, target, inventory, cast bar, key items, spells, job levels) is
read **in place** from the pattern-found globals, through bounds-checked `rd*` (section 7.1).

Open check: which guest thread calls decrypt/encrypt. If it's not the main game thread, the packet
wrap still holds the guest lock, but Lua must only be entered by one OS thread; see 6.4.

## 6. The addon host

### 6.1 Runtime

- **LuaJIT 2.1** (rolling release, pinned commit), GC64 on 64-bit hosts. One `lua_State` per addon.
- **Patched parser** for Windower's Lua dialect (our own patch, written from scratch): method
  calls on string literals (`'%d':format(n)`), indexing a table constructor, calling a function
  expression, unary `+`, unknown string escapes, lone `;`. Acceptance: every `.lua` in the corpus
  parses.
- **Built-in C modules** compiled into host64: `bit`, `ffi`, `jit` (LuaJIT), **LuaSocket**
  (`socket.core`, `mime.core`), **LuaFileSystem** (`lfs`), and a `zlib`/`struct`-style helper if
  the corpus needs it. No `package.loadlib` of foreign DLLs; a `require` of a known Windower native
  module maps to a built-in replacement or a clear error.
- State setup order: stdlibs → `xi` (native) → `xi.lua` (events, tasks, print, error policy) →
  `ashita.lua` or `windower.lua` → the addon file with chunk name `@<full path>`.

### 6.2 Folders and kinds

Under the data dir (`~/Library/Application Support/FFXIRecompile/FFXI/` on macOS):

```
addons/ashita/<name>/<name>.lua     addons/ashita/libs/...        (Ashita layout)
addons/windower/<name>/<name>.lua   addons/windower/libs/, res/   (Windower layout, exactly)
addons/native/<name>/<name>.lua     (our own xi.* API)
config/                             (Ashita addon settings: config\addons\<addon>\<name>_<id>\)
scripts/                            boot.txt and //exec scripts
```

Kind detection as VanaCore does it (count `windower.` + `_addon.` against `ashita.` + `AshitaCore`
+ `addon.`, else the folder's kind), overridable with `//addon load <name> ashita|windower`.

The installer (`tools/install.py` / the app's first run) **fetches** Ashita's `addons/libs` and
Windower's `Lua` repo and `Resources` at pinned commits. We never vendor or bundle them; they
aren't our code. Updating means bumping the pinned commits.

### 6.3 Paths

Windows-shaped paths are everywhere in addons. One path function, used by `io.open`, `io.lines`,
`os.remove`, `os.rename`, `loadfile`, `dofile`, `require`'s searchers, `lfs.*` and every `xi.fs`
call:

- `\` → `/`; strip drive letters; map `<windower path>\addons\x` → `addons/windower/x`, the Ashita
  install path → the data dir, `..\libs\` from an addon outside its tree → that kind's `libs/`.
- Case-insensitive resolution fallback (macOS volumes may be case-sensitive; Windows is not).
- `windower.windower_path`, `AshitaCore:GetInstallPath()` return Windows-style strings ending in `\`,
  because addons concatenate onto them; the path function accepts them back.

### 6.4 Errors, crashes, threads

- Every entry into Lua goes through `xpcall` with a traceback: first line to the chat log (once
  while it repeats), full trace to `host64.log`.
- Load failure → not loaded. Continuous errors for 5 s (gaps < 1 s) → unloaded.
- **Faults**: an `ffi` access outside mapped memory is a host SIGSEGV/SIGBUS (macOS) or access
  violation (Windows). A guard around each Lua entry (`sigsetjmp` + signal handler on POSIX, SEH on
  Windows) unloads that addon and abandons its state (never `lua_close` a possibly corrupt state).
- **Threading**: Lua is only entered from the thread that owns the addon host (the game's main
  thread) while holding the guest lock. Hooks that fire elsewhere (if the packet functions turn out
  to run on another guest thread) enter Lua only after taking a host recursive mutex that is always
  acquired *before* the guest lock, never after, to rule out lock-order deadlocks. `guest_call` from
  inside a Lua callback can hit a safepoint and yield the guest lock, so every Lua entry point is
  re-entrancy safe: queued events are drained, never nested more than one deep per addon.
- **Unload on exit**: addons commonly save settings in `unload`. Unload every addon on `/shutdown`,
  window close and `atexit`, before the guest is torn down (VanaCore still loses these saves).

## 7. Memory model for Lua

The hard part on a 64-bit host: a guest address is not a host pointer.

### 7.1 Addresses

Addresses Lua sees are **host addresses** (`rt_guest_base + guest`), and every API accepts either:

- `ashita.memory.find`, `windower.ffxi`'s pointers, `xi.memory.find` return host addresses, so the
  common `ffi.cast('T*', ashita.memory.find(...))` just works.
- `read_*` / `write_*` accept a number `< 2^32` as a guest address and anything inside the 4 GB
  window as a host address; outside both, reads return 0 and writes are dropped (logged once).
- Values **read from** guest memory are guest pointers (32-bit). Addons often pass them straight to
  `ffi.cast`. So the Lua layer wraps `ffi.cast`: when the target ctype is a pointer and the value is
  a nonzero number `< 2^32`, add `rt_guest_base`. (Implemented in Lua with `ffi.typeof` and a
  per-ctype cache; cheap.)
- Bounds/mapping checks replace VanaCore's `__try` copies: `gwin` knows which pages are committed.

### 7.2 Struct layouts

Addons `ffi.cdef` game structs written for 32-bit LuaJIT. Any member typed as a pointer is 8 bytes
on our host and shifts every later field. Mitigation: `ffi.cdef` is wrapped to rewrite pointer
**members** of structs (not function signatures) to `uint32_t` with a comment, and a follow-up
`ffi.cast` through 7.1 handles dereference. Corpus runs tell us whether this is needed broadly or
only for a handful of addons.

### 7.3 Code patches

Writes into FFXiMain's `.text` range succeed (it's plain RW guest memory) but change nothing,
because the recompiled C is what runs. Policy:

- Detect them (the write functions check the `.text` range): log "addon X patched code at
  0x...; ignored" once, and surface it in `//addon list`.
- Keep a **patch table**: known addon patches (by addon name + pattern) mapped to a host option or
  a wrap hook that does the same thing. Existing precedents: draw distance, fps divisor, aspect.

### 7.4 Win32 / D3D8 through `ffi.C`

`ffi.C.GetModuleHandleA`, `ffi.C` D3D8 calls, `ffi.load('user32')` don't exist on macOS. Provide an
`ffi.C` shim table for the functions the corpus uses (from our own user32/k32 shims where they
exist), and a Lua `d3d8` library replacement (device info, textures loaded from files into overlay
textures) sufficient for Ashita's ImGui addons that use it (12 of 34 in VanaCore's survey).

## 8. Our core API: `xi.*`

Primitives only; the compat layers shape them. The Ashita and Windower layers are our own,
written against the upstream APIs' documentation and source, not derived from VanaCore's.

- `xi.events.on/off` — `load`, `unload`, `command`, `present`, `text_in`, `text_out`, `packet_in`,
  `packet_out`, `key`, `mouse`, `zone`, `login`, `logout`.
- `xi.tasks` — coroutines resumed from the frame: `once`, `repeating`, `spawn`, `cancel`,
  `coroutine.sleep(s)`, `sleepf(frames)`.
- `xi.memory` — `find(module|addr, size, pattern, offset, count)`, `read_*`/`write_*` (all int
  widths, float, double, string, array), `alloc`/`free` (guest heap), `module(name)` base/size.
- `xi.game` — entity, entity_count, party, player, target, inventory, key items, spells, cast bar,
  auto-follow, recasts; per-frame read cache.
- `xi.chat` — `write(text, mode)`, `run(line, mode)`, input line get/set/open state.
- `xi.packets` — `inject_in(bytes)`, `inject_out(bytes)`, `last(dir, id)`.
- `xi.res` — items, abilities, spells, statuses, key items, zones, jobs, strings, straight from the
  DATs (section 10.3).
- `xi.ui` — text objects, rects, images (overlay renderer), `imgui` (section 11).
- `xi.input` — binds, key state, capture flags.
- `xi.fs`, `xi.config`, `xi.encoding` (Shift-JIS ↔ UTF-8 via our own tables, not iconv, so UWP
  works), `xi.time`, `xi.paths`, `xi.system.open_url`, `play_sound` (through dsound shim).

## 9. Command routing

The `parse_input` wrap calls the router with `(line, mode, injected)`:

1. Host commands: `//addon`, `/addon`, `//lua` (Windower's `lua load|unload|reload|list|command`),
   `//exec`, `//bind`, `/bind`, `//alias`, `/alias`, `//unbind`.
2. Aliases (Ashita `/alias`, Windower `alias`) expand, then re-enter the router.
3. Addons' `command` events (Ashita: every addon sees it with `blocked`; Windower: `addon command`
   for `//<name|_addon.command(s)>`).
4. Unhandled `//foo` → error line in chat, never sent to the server as chat.
5. Otherwise the original `ParseInput`.

Injected lines (`QueueCommand`, `send_command`, `input /echo`) take the same path with
`injected = true`. `windower.send_command` parses Windower's console syntax (`;`, `wait n`,
`input`, `lua ...`). `scripts/boot.txt` runs on the first frame; `//exec` runs other scripts.

## 10. Packets and game state

### 10.1 Pipeline

As VanaCore, on the plain buffer (0x1C header, then packets: u16 id:9/size:7, u16 sequence, data):

1. Fast path: no subscribers and nothing queued → untouched.
2. Per packet, in order: Ashita addons' `packet_in`/`packet_out` (original + `modified`, `blocked`,
   `injected`, chunk), then Windower `incoming chunk`/`outgoing chunk` (id, data, modified,
   injected, blocked; return a string to replace, true to block).
3. Drop blocked, append injected (padded, last sequence number, also passed through the handlers
   with `injected = true`), carry over what doesn't fit.
4. Outgoing is rebuilt in a 0x2000-byte host buffer (`gheap`), handed to the original encrypt.

### 10.2 Windower's packet-derived events and state

Windower raises most of its events from packets, not polling. We do the same (more accurate than
VanaCore's frame polling, and needed for `action`, which polling can't produce):

- A **Lua-side packet parser** built on Windower's own `packets` library field definitions
  (`libs/packets/fields.lua`), run by the host for every incoming packet when any Windower addon is
  loaded.
- Events: `login`, `logout`, `zone change`, `job change`, `target change`, `status change`,
  `hp/mp/tp/hpp/mpp change`, `gain buff`, `lose buff`, `action` (0x028 decoded into Windower's
  action table), `action message`, `party invite`, `time change`, `day change`, `weather change`,
  `linkshell change`, `emote`, `examined`, `incoming/outgoing text`, `chat message`,
  `add item`/`remove item`, `level up/down`, `gain experience`, `ipc message` (between our
  addons), `keyboard`, `mouse`, `prerender`, `postrender`.
- `windower.ffxi.*` getters filled from game memory where that's authoritative and from a
  packet-fed state block where Windower's are (items per bag, key items, merits, job points,
  recasts, mjob data). Implement `get_items`, `get_spells`, `get_abilities`, `get_key_items`,
  `get_ability_recasts`, `get_spell_recasts`, `get_mjob_data`, `get_info` in full.
- `windower.packets.*` (`inject_incoming/outgoing`, `last_incoming/outgoing`) over `xi.packets`;
  the `pack` library is pure Lua over `string.pack`-like primitives → provide Windower's `pack`
  natively in C (it is a C module in Windower).

### 10.3 Resources

- **Ashita** (`AshitaCore:GetResourceManager()`): items, abilities, spells, statuses, strings,
  from the DATs, per VanaCore 6.2.6 (item records rotate-right-5, `d_msg` XOR, spdata
  `DecodeDataBlockMask`). We already read DATs for UI (`host/datui.c`), so the VTABLE/FTABLE mapper
  is shared.
- **Windower** `res` library reads `res/*.lua` files from Windower's `Resources` repo, fetched at
  install time. Option B later: generate them from the DATs ourselves so they match the private
  server's data (CatsEyeXI and other LSB servers differ from retail).

## 11. Overlay: ImGui, text, primitives

Nothing like this exists today (the FPS counter is a Metal-only shader), so it is new.

- `runtime/portable/overlay.c` draws on **`gfx.h` only** (textured, alpha-blended XYZRHW triangles),
  so Metal and D3D12 both get it without back-end work. Drawn from the Present hook after the game's
  frame, in the game's pixels (it follows the UI squeeze/scale rules from `ui_aspect`).
- **Dear ImGui**, one context for everything, rendered by a small ImGui backend over `gfx.h`. This
  brings C++ into host64: `build_posix.py` and `build.py` compile `.cpp` with clang++ / cl and link
  `-lc++`. The rest of host64 stays C and talks to ImGui through a thin `extern "C"` wrapper.
  - **Version**: Ashita 4.30 addons use ImGui 1.92; 4.16-era addons use 1.81 names. Decided:
    **ImGui 1.92**, report interface 4.30 to `PluginManager:Get('addons')`, and add a 1.81 alias
    table in the Lua binding for renamed functions and enums. (VanaCore pinned 1.81 because native
    plugins pass ImGui structs by value; we host no native plugins, so we're free to choose.)
  - **Lua binding**: generated from dear_bindings' JSON for 1.92, following Ashita's conventions
    exactly (VanaCore 5.5): pointer args are tables (`t[1]`), ImVec in as `{x,y}` / out as
    multiple returns, 1-based live `GetStyle().Colors`, draw lists with `Add*`, negative ImU32
    colours, printf functions take the string alone, missing function = `nil`.
  - Misuse can't crash: `IM_ASSERT` logs once and continues; unbalanced Begin/End and pushes are
    closed at frame end; draw calls outside a frame are ignored.
  - Fonts: Ashita's defaults (Agave + Font Awesome 6), atlas rebuilt between frames when addons add
    fonts.
- **Text objects** (Ashita `fonts` / `AshitaCore:GetFontManager()`, Windower `windower.text` and
  the `texts` library): rasterised with **stb_truetype** into a glyph atlas (cross-platform, no
  CoreText/DirectWrite split). Font family names map to bundled fonts plus system font files found
  by name; sizes in points for Windower, pixels for Ashita; outline, background, padding, right
  justify, `\cs(r,g,b)`/`\cr`, parents and anchors, drag with Shift, `block_mouse`.
- **Primitives** (Ashita primitives, `windower.prim`, `images` library): solid or textured quads;
  images loaded with `stb_image` (already vendored), fit or tile.
- Draw order: game → Windower `prerender` → text/prims in creation order → ImGui → `postrender`.

## 12. Input, binds and capture

We own DirectInput, user32 and the SDL pump, so capture is simpler than on Windows:

- The SDL pump asks the overlay first. Mouse over an ImGui window / a `block_mouse` text / an addon
  that returned true from `mouse` → the message isn't posted, and `dinput.c` zeroes buttons and
  wheel in `GetDeviceState` and drops those `GetDeviceData` events (X/Y still flow). A press that
  starts over the overlay stays the overlay's until release.
- ImGui text focus → no key presses reach DirectInput (releases still do), and SDL text input goes
  to ImGui instead of `WM_CHAR`.
- `key` (Ashita) / `keyboard` (Windower, DIK code, down, flags, blocked) events fire from the
  DirectInput layer; returning true hides the key from the game until release.
- **Binds**: a host bind table keyed by DIK code + modifiers (Ashita `/bind` syntax: `^` ctrl,
  `!` alt, `@` win/cmd, `#` apps, `+` shift, `%` only while chat closed, `$` only while chat open;
  Windower's `bind` syntax maps to the same table). A bound key is consumed and runs its command
  through the router. On macOS, `@` maps to Cmd.
- XInput / controller events for Ashita's `xinput_*` events come from `XI_GetState`.

## 13. Compat layer rules

- **Loud gaps**: every unimplemented member of `AshitaCore` managers, `ashita.*`, `windower.*`
  raises `"<path> is not supported yet"` via a `partial()` metatable (VanaCore 5.3), except where
  addons feature-test (ImGui functions, `windower.ffxi.get_*` optional fields), which return `nil`.
  Every hit is also counted per addon and written to `addons-compat.log`.
- Frame caching for polled getters (Ashita addons call one getter per field per member per frame).
- Ashita `settings` library, per-character config paths, Windower `config` library: unchanged, fed
  by correct install/character paths.

## 14. Test harness

The survey method is what makes "fully" measurable:

- **Headless host**: `host64 --gfx null --replay <capture>` runs the real translated game with
  `gfx_null.c`, feeds a recorded packet stream through the decrypt wrap (captured with a
  `--record-packets` switch from real sessions on our servers), and fakes input.
- **Corpus runner** (`tools/addon_survey.py`): for each addon, load it, run N seconds of replay,
  issue its documented commands, unload; collect Lua errors, `unsupported` hits, faults and code
  patches into one table. CI fails on regressions in the pass list.
- Parser test: every corpus `.lua` parses under the patched LuaJIT.
- ImGui binding test: call every bound function with representative args inside a frame.

## 15. Native plugins later (not v1)

Option ranking, for when there's demand:

1. Reimplement the popular ones (addon or host feature).
2. **Recompile plugin DLLs** with our recompiler as extra modules (`rt_add_module` already exists
   for FFXi.dll). The host then needs Ashita's SDK vtables in guest memory with thunks
   (`thunk_register`), generated from the SDK headers with an `unimplemented` default, plus MSVC C++
   EH support in the runtime. Large, but no x86 execution needed.
3. An interpreter for plugin code. Not recommended.

## 16. Platform notes

- **macOS**: LuaJIT's JIT needs executable pages. Fine today (no hardened runtime); if we notarize
  with the hardened runtime, add `com.apple.security.cs.allow-jit` (LuaJIT uses `MAP_JIT` on
  arm64 macOS).
- **Xbox/UWP (dev mode)**: the package has no `codeGeneration` capability. Either add it or run
  LuaJIT with the JIT off (`jit.off()` globally; the interpreter is still quick). Default: JIT off
  on UWP.
- **Windows x64**: straightforward; MSVC builds LuaJIT with `msvcbuild.bat` or our manifest build.
- LuaJIT, LuaSocket, lfs, stb_truetype, Dear ImGui are vendored through `tools/vendor.py` /
  `thirdparty.py` manifests like SDL3 and mbedtls. LuaJIT needs its `buildvm` step; add a
  "host tool then generate" stage to `thirdparty.py`.

## 17. Phases

| Phase | Deliverable | Proof |
|---|---|---|
| 0 | `--wraps` in the recompiler; `parse_input`, `write_line`, `packet_decrypt/encrypt` in `builds.json` for all three builds; `newbuild.py` finds them by pattern | `//echo`-style host command handled; chat line printed from host |
| 1 | LuaJIT (patched) vendored; addon host, `xi.*`, paths, errors, tasks, boot script; overlay text + rects on `gfx.h` | a native `xi` addon draws text and reacts to commands, on Metal and D3D12 |
| 2 | Ashita layer (managers, memory model, events incl. `text_in/out`, `packet_in/out`, `key`, `mouse`), fonts/primitives, ImGui 1.92 + generated binding, resources from DATs | Ashita official addons: majority pass in the survey |
| 3 | Windower layer, packet-derived events and state, `pack`, `windower.prim`, `texts`/`images`, `res` fetch | Windower Lua repo: majority pass |
| 4 | Binds/aliases, code-patch table, `ffi.C`/`d3d8` shims, LuaSocket/lfs, headless survey in CI, README rewrite | corpus pass rate at target, exception list published |
| 5 (optional) | Native plugin recompilation or reimplementations | per-plugin |

## 18. Decisions and open questions

Decided (2026-09-30):

1. **Our own layers.** `xi.*`, `ashita.lua`, `windower.lua`, the LuaJIT dialect patch and the ImGui
   binding generator are written here, not taken from VanaCore. Vekien's write-up is a reference for
   behaviour and pitfalls only.
2. **ImGui 1.92** with a 1.81 alias table; report interface 4.30.
3. **Fetch** Ashita/Windower libs and Windower `Resources` at install time, pinned by commit.

Open:

1. Which thread calls packet decrypt/encrypt (verify in a session; drives 6.4).
2. Corpus membership: which third-party addons count toward "fully".
