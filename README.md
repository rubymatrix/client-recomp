# ffxiv-123b-macOS

![In game](screenshots/ffxiv-in-game.webp)

FINAL FANTASY XIV 1.23b (the original 1.0 client) running natively on Apple silicon Macs. No Wine,
no Rosetta.

The game's 32-bit x86 executable is statically translated to C on your machine, from your own
install, and compiled as an arm64 macOS app. Windows, Direct3D 9, audio, input and networking are
replaced by a macOS platform layer (Direct3D 9 runs on Metal).

It connects to 1.23b private servers: [Project Meteor](https://github.com/Ioncannon/project-meteor-server),
[Bahamut](https://github.com/BahamutXIV/bahamut-launcher) and [Garlemald](https://github.com/swstegall/Garlemald-Server).

## Requirements

- Apple silicon Mac, Xcode command-line tools, Python 3
- Your own FINAL FANTASY XIV install patched to 1.23b
- A running 1.23b server

## Build

```sh
python3 -m venv .venv && .venv/bin/pip install pycryptodome
.venv/bin/python tools/build_xiv.py translate --exe /path/to/ffxivgame.exe
.venv/bin/python tools/build_xiv.py xivboot   --exe /path/to/ffxivgame.exe
```

This produces `build/xivboot`.

## Install from the retail disc

Mount the FINAL FANTASY XIV disc (or its image) and double-click `install.command`, or:

```sh
curl -fsSL https://raw.githubusercontent.com/rubymatrix/client-recomp/main/install.command | bash -s -- \
    --server <host> --account-url http://<host>:8090
```

It gets Apple's command line tools if they are missing, puts these sources in
`~/Library/Application Support/FFXIV1_0/source`, unpacks the disc's InstallShield cabinets natively
(none of the disc's code runs) into `~/Library/Application Support/FFXIV1_0/FINAL FANTASY XIV`,
translates and compiles the disc's `ffxivboot.exe` and `ffxivupdater.exe`, and puts
`FINAL FANTASY XIV.app` in `/Applications`. The app starts the native ffxivboot: it checks the
version with the server, downloads the patches and has the native updater apply them up to 1.23b;
then ffxivlogin signs in and the game starts. Each program is translated again when a patch
replaces it (`tools/xiv_run.py`); the game is built while you sign in. The server runs
[meteor-web](https://github.com/rubymatrix/Project-Meteor-Web) for the version check, patches and
login page; the installer fetches its patch signing certificate from it (`/patch/cert`).

`tools/xiv_e2e.sh` tests all of it end to end, from the disc to the lobby's character list, in
folders of its own (a fresh clone, install, Documents and app folder).

## Play

```sh
.venv/bin/python tools/xiv_login.py --meteor --server http://127.0.0.1:8000 --user NAME \
    --game "/path/to/FINAL FANTASY XIV"
```

Signs in through the server's web login and starts the game: Project Meteor with `--meteor`,
Bahamut with `--bahamut`, Garlemald otherwise. `--lobby` sets the lobby host. On the public Bahamut
server:

```sh
.venv/bin/python tools/xiv_login.py --bahamut --server https://bahamut.stegall.me:443 --lobby bahamut.stegall.me \
    --user NAME --game "/path/to/FINAL FANTASY XIV"
```

## No game files here

This repo contains no Square Enix code or data, and never will. The translated C is generated
locally from your own copy of the game and is not committed.

## Credits

The recompiler and runtime come from
[xi-on-anything](https://github.com/rubymatrix/xi-on-anything), the same approach applied to
FINAL FANTASY XI.

## License

MIT (see `LICENSE`). FINAL FANTASY is a registered trademark of Square Enix Holdings Co., Ltd.
This project is not affiliated with or endorsed by Square Enix.
