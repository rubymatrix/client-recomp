"""Survey Windower addons in the addon harness (host64 --addon-harness).

    python3 tools/windower_survey.py --data-dir <dir> [--game <FFXI dir>] [--host build/host64]
                                     [--lua host/addons/lua] [--only a,b] [--markdown out.md] [-v]

<dir>/windower/ is a Windower install layout (addons/<name>/<name>.lua, addons/libs/, res/). For
each addon: load it, run frames, a few seconds of time, its command with no arguments and with
"help", a handful of synthetic packets, key and mouse events and chat lines, then unload. The
table: loaded or not, the first error line, and the "unsupported: ..." hits the Windower layer
logged (host/addons/lua/windower.lua).

The game's structures are empty in the harness (nobody is logged in), so addons that need a
logged-in player at load (and fail with nil errors) are expected; the table says so.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# a few packets through the pipeline (header included; the harness fixes the size field)
PACKETS_IN = [
    '0a00 0000' + '00' * 0x100,          # zone in (mostly zeros)
    '1700 0000' + '00' * 0x40,           # chat message
    '2800 0000' + '00' * 0x40,           # action
    '6100 0000' + '00' * 0x50,           # char stats
    '6300 0000' + '09 00' + '00' * 0x40,  # set update
]
PACKETS_OUT = [
    '1500 0000' + '00' * 0x14,           # position
    '1a00 0000' + '00' * 0x1c,           # action
]


def command_names(src, name):
    names = []
    for m in re.finditer(r"_addon\.commands?\s*=\s*(\{[^}]*\}|'[^']*'|\"[^\"]*\")", src):
        names += re.findall(r"['\"]([^'\"]+)['\"]", m.group(1))
    return names or [name]


def script_for(name, cmd):
    lines = [
        f'load {name} windower',
        'frames 5',
        f'command //{cmd}',
        f'command //{cmd} help',
        'frames 3',
        'text_in 1 Hello there',
        'text_in 121 A test line',
        'command /echo testing',
        'key 1e 1',
        'key 1e 0',
        'mouse 200 100 100',
        'mouse 201 100 100',
        'mouse 202 100 100',
    ]
    lines += [f'packet_in {p}' for p in PACKETS_IN]
    lines += [f'packet_out {p}' for p in PACKETS_OUT]
    lines += ['run 0.5', 'frames 3', f'unload {name}']
    return '\n'.join(lines) + '\n'


def survey(args, name, main):
    src = open(main, encoding='latin-1').read()
    cmd = command_names(src, name)[0]
    with tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False) as f:
        f.write(script_for(name, cmd))
        script = f.name
    env = dict(os.environ)
    if args.lua:
        env['FFXI_ADDONS_LUA'] = os.path.abspath(args.lua)
    try:
        p = subprocess.run([args.host, '--game', args.game, '--data-dir', args.data_dir, '--addon-harness', script],
                           cwd=ROOT, env=env, capture_output=True, timeout=args.timeout)
        out, err, code = p.stdout.decode('latin-1'), p.stderr.decode('latin-1'), p.returncode
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b'').decode('latin-1')
        err = (e.stderr or b'').decode('latin-1')
        code = 'timeout'
    finally:
        os.unlink(script)
    loaded = f'addon: loaded {name} (windower)' in out
    errors = []
    for line in out.splitlines():
        m = re.match(r'\[chat \d+\] \[' + re.escape(name) + r'\] error: (.*)', line)
        if m and m.group(1) not in errors:
            errors.append(m.group(1))
    unsupported = []
    for line in err.splitlines():
        m = re.search(re.escape(name) + r': unsupported: (.*)', line)
        if m and m.group(1) not in unsupported:
            unsupported.append(m.group(1))
    crashed = 'crashed in native code' in out or (code not in (0, 1))
    return {'name': name, 'loaded': loaded, 'errors': errors, 'unsupported': unsupported, 'crashed': crashed,
            'code': code, 'out': out, 'err': err}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data-dir', required=True)
    ap.add_argument('--game', required=True)
    ap.add_argument('--host', default=os.path.join(ROOT, 'build', 'host64'))
    ap.add_argument('--lua', default=os.path.join(ROOT, 'host', 'addons', 'lua'))
    ap.add_argument('--only', default='')
    ap.add_argument('--timeout', type=float, default=60)
    ap.add_argument('--markdown')
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args()

    addons = os.path.join(args.data_dir, 'windower', 'addons')
    only = {n.lower() for n in args.only.split(',') if n}
    rows = []
    for name in sorted(os.listdir(addons), key=str.lower):
        if name == 'libs' or (only and name.lower() not in only):
            continue
        main_file = os.path.join(addons, name, name + '.lua')
        if not os.path.isfile(main_file):
            cands = [f for f in os.listdir(os.path.join(addons, name)) if f.lower() == name.lower() + '.lua'] \
                if os.path.isdir(os.path.join(addons, name)) else []
            if not cands:
                continue
            main_file = os.path.join(addons, name, cands[0])
        r = survey(args, name, main_file)
        rows.append(r)
        if args.verbose:
            print(r['out'])
            print('\n'.join(l for l in r['err'].splitlines() if '[addons]' in l))
        status = 'ok' if r['loaded'] and not r['errors'] and not r['crashed'] else \
            'CRASH' if r['crashed'] else 'loaded, errors' if r['loaded'] else 'FAILED'
        r['status'] = status
        print(f"{name:24} {status:15} {(r['errors'][0] if r['errors'] else '')[:110]}"
              + (f"  [unsupported: {', '.join(r['unsupported'])}]" if r['unsupported'] else ''), flush=True)

    total = len(rows)
    ok = sum(1 for r in rows if r['status'] == 'ok')
    loaded = sum(1 for r in rows if r['loaded'])
    print(f'\n{total} addons: {loaded} loaded, {ok} clean (no errors, no crash), '
          f"{sum(1 for r in rows if r['unsupported'])} hit unsupported members")
    if args.markdown:
        with open(args.markdown, 'w') as f:
            f.write('| addon | status | first error | unsupported |\n|---|---|---|---|\n')
            for r in rows:
                err = (r['errors'][0] if r['errors'] else '').replace('|', '\\|')[:160]
                f.write(f"| {r['name']} | {r['status']} | {err} | {', '.join(r['unsupported'])} |\n")
    return 0


if __name__ == '__main__':
    sys.exit(main())
