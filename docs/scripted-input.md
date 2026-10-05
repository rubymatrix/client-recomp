# Scripted input (`XIV_INPUT_SCRIPT`)

Drives the client without a human: a timeline of key presses, gamepad buttons and sticks, mouse
clicks and screenshots, played into the game as if a player made them. Used for unattended
login + in-world tests.

Code: `runtime/portable/autoinput.c` (timeline, ticked once per presented frame from
`d3d9.c present()`), scripted pad in `runtime/portable/input.c`, window/cursor helpers in
`runtime/portable/user32.c`.

## How the input reaches the game

The client reads the keyboard three ways: window messages (`WM_KEYDOWN`/`WM_CHAR`),
`GetKeyboardState`, and DirectInput8 (`GetDeviceState` and the buffered `GetDeviceData`). The
gamepad is enumerated through DirectInput and, being an Xbox-style pad, read through XInput
(`xinput1_3` ordinal 2).

- **Keys and mouse** are synthesized SDL events pushed onto SDL's queue (`SDL_PushEvent`). The
  window pump turns them into all of the above exactly as it does real ones, so `down` holds a
  key everywhere until `up`. (The old `XIV_AUTOKEYS` path only posted `WM_KEYDOWN`+`WM_KEYUP`,
  which the menus ignore.)
- **Gamepad**: while `XIV_INPUT_SCRIPT` (or `XIV_VPAD=1`) is set, pad 0 exists from start-up as
  "Scripted pad" (DirectInput enumeration, XInput and DirectInput state). What the script holds is
  added on top of a real pad 0 if one is connected. `XIV_VPAD=0` turns the scripted pad off.
- **Mouse**: `click`/`move` also override what `GetCursorPos` reports.

## Script format

One action per line; `#` starts a comment.

```
<time> <command> [args]
```

`<time>` is milliseconds since the first frame, or `+ms` after the previous line. The file is
re-read every 100 ms: **lines appended while the game runs are played too**, and a `+ms` time on
an appended line counts from when it is read (so `+0` means now).

| command | effect |
|---|---|
| `key K [hold_ms]` | press K, release after hold (default 120) |
| `down K` / `up K` | hold / release K |
| `pad BTN [hold_ms]` | press a pad button (default 150): `a b x y lb rb back start ls rs up down left right` |
| `paddown BTN` / `padup BTN` | hold / release |
| `stick l\|r X Y [hold_ms]` | stick position, -1..1, Y up = forward; without hold it stays until the next `stick` |
| `trigger l\|r V [hold_ms]` | trigger 0..1 |
| `move X Y` | cursor to X,Y as fractions (0..1) of the window |
| `click X Y [left\|right\|middle] [hold_ms]` | move there and click |
| `shot NAME` | back buffer to `<dir>/NAME.ppm` (`XIV_INPUT_SHOTS`, else the script's folder); `sips -s format png NAME.ppm --out NAME.png` |
| `log TEXT` | `[input] <ms>: TEXT` in the client log (sync point for shell loops) |
| `quit` | exit the client |

K is an SDL key name (`Return`, `Escape`, `Left`, `W`, `Space`, `0`) or a Win32 virtual-key code
of two or more digits / hex (`0x0D`, `13`). Every action is logged as `[input] <ms>: ...`.

## Menu facts (1.23b, measured)

- Title: `Return` on Start opens "A Word to Our Players" with **No** highlighted: `Left`, then
  `Return`. Pad `A` on that dialog went back to the title (do not use it there).
- Character select opens on the first character; `Return` goes straight to loading (no confirm).
- In world, left stick and `W/A/S/D` both walk (camera-relative); the server sees 0x00CA position
  updates.

## Running it

```bash
cd ~/Projects/FFXIV1_0
./xivctl launch --no-build --input recomp/tools/input/login_walk.txt   # copies to recomp/build/run/input.txt
./xivctl wait 180                                                        # server-side: zoned in
./xivctl input '+0 stick l 0 1 3000' '+3500 shot after'                  # queue more, any time
```

Screenshots of an `xivctl` run land in `recomp/build/run/`. `recomp/tools/input/login_walk.txt`
is title -> character 1 -> world -> 3 s forward; its waits were measured on an M1 Max and may need
raising on a slower machine (look at its `0N_*.ppm` shots).
