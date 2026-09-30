#!/usr/bin/env python3
"""Addon compatibility survey: loads every addon of a kind in the headless harness, one at a time.

    python3 tools/addon_survey.py --kind ashita --data-dir <dir> [--game <FFXI folder>]
        [--host build/host64] [--lua host/addons/lua] [--only a,b] [--jobs 6] [--json out.json]
        [--markdown out.md] [--keep-logs <dir>]

For each addon under <data dir>/<kind>/addons/<name>/<name>.lua the harness (host64
--addon-harness, host/addons/harness.c) runs a generic script: load, frames, the addon's own
commands (read from its source: '/name' strings next to args[1], sub-commands from args[2] tests;
Windower: its _addon.command(s)), some text, key and mouse events, a second of frames, unload.

The table: loaded ok, Lua errors (count and the first line), "unsupported:" hits (the compat
layers log one line per missing member), native faults. Exit status 1 if any addon failed to load.

A test fixture is loaded first (an xi addon, written into the data dir as survey_seed): it gives
the game the few objects a running game would have that Ashita's libs read at load time (the
CYmdb resolution object libs/scaling.lua needs). Nothing in an addon's own path is faked.
"""
import argparse
import concurrent.futures
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SEED = r"""-- survey fixture (tools/addon_survey.py): what a running game has that the harness doesn't
local p = xi.memory.find('FFXiMain.dll', 0, 'A1????????85C05E74????80', 1, 0)
if p ~= 0 then
    local slot = xi.memory.read_uint32(p)
    if slot ~= 0 and xi.memory.read_uint32(slot) == 0 then
        local obj = xi.memory.alloc(0x40)
        xi.memory.write_uint16(obj + 0x10, 1920)
        xi.memory.write_uint16(obj + 0x12, 1080)
        xi.memory.write_uint16(obj + 0x14, 1280)
        xi.memory.write_uint16(obj + 0x16, 720)
        xi.memory.write_uint32(slot, xi.memory.guest(obj))
    end
end
"""

GENERIC_TAIL = """frames 5
text_in 1 survey: a line for the chat log
key 1c 1
key 1c 0
mouse 200 100 100
mouse 201 100 100
mouse 202 100 100
packet_in {zone_in}
packet_in {chat}
packet_out {position}
run 1
"""

# whole packets as the server/client send them (the harness writes the size field)
ZONE_IN = '0a00' + '00' * 0x102                                       # 0x00A zone in: zeros
CHAT = '1700' + '00' * 2 + '00' + '00' * 3 + ''.join('%02x' % ord(c) for c in 'Survey'.ljust(15, '\0')) + \
    ''.join('%02x' % ord(c) for c in 'hello from the survey'.ljust(0x100 - 0x18, '\0'))  # 0x017 chat
POSITION = '1500' + '00' * 0x1E                                        # 0x015 position
GENERIC_TAIL = GENERIC_TAIL.format(zone_in=ZONE_IN, chat=CHAT, position=POSITION)


def lua_files(folder):
    out = []
    for dirpath, _, files in os.walk(folder):
        for f in files:
            if f.endswith('.lua'):
                out.append(os.path.join(dirpath, f))
    return out


def read(path):
    with open(path, 'rb') as f:
        return f.read().decode('latin-1')


def ashita_commands(folder, limit_sub=6):
    """'/cmd' strings the addon tests args[1] against, and its args[2] sub-commands."""
    cmds, subs = [], []
    for path in lua_files(folder):
        src = read(path)
        for line in src.splitlines():
            if 'args[1]' in line or 'command' in line and 'any(' in line:
                for m in re.finditer(r"""['"](/[A-Za-z][\w-]*)['"]""", line):
                    if m.group(1) not in cmds:
                        cmds.append(m.group(1))
            if 'args[2]' in line:
                for m in re.finditer(r"""args\[2\]\s*(?::any\(([^)]*)\)|:ieq\(([^)]*)\)|==\s*(['"][^'"]*['"]))""", line):
                    body = next(g for g in m.groups() if g)
                    first = re.search(r"""['"]([^'"]+)['"]""", body)
                    if first and first.group(1) not in subs:
                        subs.append(first.group(1))
    return cmds[:3], subs[:limit_sub]


def windower_commands(main_src):
    names = []
    m = re.search(r"_addon\.commands?\s*=\s*(\{[^}]*\}|['\"][^'\"]*['\"])", main_src)
    if m:
        names = re.findall(r"['\"]([^'\"]+)['\"]", m.group(1))
    return ['//' + n for n in names[:1]], []


def script_for(kind, name, folder, main):
    lines = ['load survey_seed xi', 'load %s %s' % (name, kind), 'frames 3']
    if kind == 'ashita':
        cmds, subs = ashita_commands(folder)
    elif kind == 'windower':
        cmds, subs = windower_commands(read(main))
    else:
        cmds, subs = [], []
    for c in cmds:
        lines.append('command ' + c)
        lines.append('frames 2')
    if cmds:
        for s in subs:
            if s in ('unload', 'reload', 'rl'):
                continue
            lines.append('command %s %s' % (cmds[0], s))
            lines.append('frames 2')
    lines.append(GENERIC_TAIL.strip())
    lines.append('unload ' + name)
    lines.append('frames 2')
    return '\n'.join(lines) + '\n', cmds, subs


def survey_one(args, kind, name):
    folder = os.path.join(args.data_dir, kind, 'addons', name)
    main = os.path.join(folder, name + '.lua')
    text, cmds, subs = script_for(kind, name, folder, main)
    work = os.path.join(args.data_dir, '.survey')
    os.makedirs(work, exist_ok=True)
    spath = os.path.join(work, name + '.txt')
    with open(spath, 'w') as f:
        f.write(text)
    env = dict(os.environ)
    if args.lua:
        env['FFXI_ADDONS_LUA'] = args.lua
    cmd = [args.host, '--game', args.game, '--data-dir', args.data_dir, '--addon-harness', spath]
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, timeout=args.timeout)
        out, code = p.stdout.decode('utf-8', 'replace'), p.returncode
    except subprocess.TimeoutExpired as e:
        out, code = (e.stdout or b'').decode('utf-8', 'replace'), 'timeout'
    if args.keep_logs:
        os.makedirs(args.keep_logs, exist_ok=True)
        with open(os.path.join(args.keep_logs, name + '.log'), 'w') as f:
            f.write(text + '\n---\n' + out)
    return analyse(name, out, code, cmds, subs)


def analyse(name, out, code, cmds, subs):
    lname = re.escape(name)
    loaded = re.search(r'addon: loaded %s \(' % lname, out, re.I) is not None
    errors = []
    unsupported = []
    faults = 0
    for line in out.splitlines():
        m = re.match(r'\[addons\] %s: unsupported: (.*)$' % lname, line, re.I)
        if m:
            if m.group(1) not in unsupported:
                unsupported.append(m.group(1))
            continue
        m = re.match(r'\[addons\] %s: native fault' % lname, line, re.I)
        if m:
            faults += 1
            continue
        m = re.match(r'\[addons\] %s: ([^:]+): (.*)$' % lname, line, re.I)
        if m and not m.group(2).startswith('unsupported:'):
            where, msg = m.group(1), m.group(2)
            if where in ('loaded', 'unloaded') or msg.startswith('from '):
                continue
            errors.append('%s: %s' % (where, msg))
    return {
        'name': name, 'loaded': loaded, 'exit': code, 'errors': len(errors),
        'first_error': errors[0] if errors else '', 'unsupported': unsupported, 'faults': faults,
        'commands': cmds, 'subcommands': subs,
    }


def short(s, n=110):
    s = re.sub(r'\S*/(addons/[^:]+)', r'\1', s)
    return s if len(s) <= n else s[:n - 3] + '...'


def markdown(rows):
    out = ['| addon | loaded | errors | first error | unsupported | faults |',
           '|---|---|---|---|---|---|']
    for r in rows:
        out.append('| %s | %s | %d | %s | %s | %d |' % (
            r['name'], 'yes' if r['loaded'] else '**no**', r['errors'], short(r['first_error']).replace('|', '\\|'),
            ', '.join(r['unsupported']) or '', r['faults']))
    ok = sum(1 for r in rows if r['loaded'] and not r['errors'] and not r['unsupported'] and not r['faults'])
    out.append('')
    out.append('%d addons: %d loaded, %d clean (loaded, no errors, no unsupported hits, no faults)' % (
        len(rows), sum(1 for r in rows if r['loaded']), ok))
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--kind', default='ashita', choices=['ashita', 'windower', 'xi'])
    ap.add_argument('--data-dir', required=True)
    ap.add_argument('--game', default=os.path.join(os.path.dirname(ROOT), 'FINAL FANTASY XI'))
    ap.add_argument('--host', default=os.path.join(ROOT, 'build', 'host64'))
    ap.add_argument('--lua', default=None, help='FFXI_ADDONS_LUA: the layers from this folder instead of the build')
    ap.add_argument('--only', default=None)
    ap.add_argument('--jobs', type=int, default=6)
    ap.add_argument('--timeout', type=int, default=120)
    ap.add_argument('--json', default=None)
    ap.add_argument('--markdown', default=None)
    ap.add_argument('--keep-logs', default=None)
    args = ap.parse_args()
    args.data_dir = os.path.abspath(args.data_dir)

    seed = os.path.join(args.data_dir, 'xi', 'addons', 'survey_seed')
    os.makedirs(seed, exist_ok=True)
    with open(os.path.join(seed, 'survey_seed.lua'), 'w') as f:
        f.write(SEED)

    base = os.path.join(args.data_dir, args.kind, 'addons')
    names = sorted(n for n in os.listdir(base)
                   if n != 'libs' and os.path.isfile(os.path.join(base, n, n + '.lua')))
    if args.only:
        want = set(args.only.split(','))
        names = [n for n in names if n in want]
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        rows = list(ex.map(lambda n: survey_one(args, args.kind, n), names))
    md = markdown(rows)
    print(md)
    if args.markdown:
        with open(args.markdown, 'w') as f:
            f.write(md + '\n')
    if args.json:
        with open(args.json, 'w') as f:
            json.dump(rows, f, indent=1)
    return 0 if all(r['loaded'] for r in rows) else 1


if __name__ == '__main__':
    sys.exit(main())
