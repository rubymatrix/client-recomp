"""Build the vendored libraries (third_party/<name>) with plain clang.

  python3 tools/thirdparty.py [sdl3|mbedtls|luajit|imgui|luasocket|lfs ...]

Each library's third_party/<name>/manifest.json (written by tools/vendor.py) lists its sources in
groups with their include folders and flags. This compiles them into build/third_party/<name>.a,
once: the archive is rebuilt only when the manifest changes. build_posix.py links host64 against
the archives with flags() and libs(), so a player's Mac needs clang and nothing else.

A manifest with "kind": "make" (LuaJIT) is built by the library's own makefile instead, in a copy
of its tree under build/third_party/ (LuaJIT builds and runs its own host tools: minilua, buildvm).
C++ sources (.cpp: Dear ImGui) compile with clang++.
"""
import concurrent.futures
import hashlib
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
NAMES = ('sdl3', 'mbedtls', 'luajit', 'imgui', 'luasocket', 'lfs')
MIN_MACOS = '12.0'


def manifest(name):
    with open(os.path.join(ROOT, 'third_party', name, 'manifest.json')) as f:
        return json.load(f)


def archive(name):
    return os.path.join(ROOT, 'build', 'third_party', name + '.a')


def flags(name):
    """What code that includes the library's headers compiles with."""
    return ['-I' + os.path.join(ROOT, 'third_party', name, d) for d in manifest(name)['public']]


def libs(name):
    m = manifest(name)
    return ([archive(name)] + sum((['-framework', f] for f in m['frameworks']), [])
            + sum((['-weak_framework', f] for f in m['weak_frameworks']), []))


def tree_stamp(name):
    """The newest modification time in third_party/<name>: our own edits to a vendored library (the
    LuaJIT parser) rebuild it."""
    newest = 0
    for d, _, files in os.walk(os.path.join(ROOT, 'third_party', name)):
        for f in files:
            newest = max(newest, os.path.getmtime(os.path.join(d, f)))
    return '%.0f' % newest


def build_make(name, m, out, stamp, key):
    """LuaJIT: its makefile, static, in a copy of third_party/<name> (the tree stays clean)."""
    import shutil
    work = os.path.join(ROOT, 'build', 'third_party', name + '-src')
    if os.path.exists(work):
        shutil.rmtree(work)
    shutil.copytree(os.path.join(ROOT, 'third_party', name), work)
    env = dict(os.environ, MACOSX_DEPLOYMENT_TARGET=MIN_MACOS)
    r = subprocess.run(['make', '-C', 'src', '-j%d' % (os.cpu_count() or 4), 'BUILDMODE=static', 'CC=clang',
                        'XCFLAGS=-DLUAJIT_ENABLE_LUA52COMPAT', 'libluajit.a'],
                       cwd=work, env=env, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit('%s: make failed:\n%s' % (name, (r.stdout + r.stderr)[-3000:]))
    shutil.copyfile(os.path.join(work, 'src', 'libluajit.a'), out)
    with open(stamp, 'w') as f:
        f.write(key)
    return out


def build(name, progress=None):
    """Compile the library into build/third_party/<name>.a unless it is up to date."""
    m = manifest(name)
    lib = os.path.join(ROOT, 'third_party', name)
    out = archive(name)
    stamp = out + '.stamp'
    key = hashlib.sha256(json.dumps(m, sort_keys=True).encode() + MIN_MACOS.encode() + tree_stamp(name).encode()).hexdigest()
    if os.path.exists(out) and os.path.exists(stamp) and open(stamp).read() == key:
        return out
    os.makedirs(os.path.join(ROOT, 'build', 'third_party'), exist_ok=True)
    if m.get('kind') == 'make':
        return build_make(name, m, out, stamp, key)
    objdir = os.path.join(ROOT, 'build', 'third_party', name)
    os.makedirs(objdir, exist_ok=True)
    jobs = []
    for n, g in enumerate(m['groups']):
        base = (['clang', '-O2', '-DNDEBUG', '-w', '-mmacosx-version-min=' + MIN_MACOS] + g['flags']
                + ['-I' + os.path.join(lib, d) for d in g['include']]
                + sum((['-idirafter', os.path.join(lib, d)] for d in g['idirafter']), []))
        for src in g['sources']:
            obj = os.path.join(objdir, '%d_%s.o' % (n, src.replace('/', '_')))
            arc = ['-fobjc-arc'] if src.endswith('.m') else []
            cmd = base + arc + ['-c', os.path.join(lib, src), '-o', obj]
            if src.endswith('.cpp'):
                cmd[0] = 'clang++'
            jobs.append((obj, cmd))
    done = 0
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as ex:
        for obj, r in zip((j[0] for j in jobs),
                          ex.map(lambda j: subprocess.run(j[1], capture_output=True, text=True), jobs)):
            if r.returncode:
                raise SystemExit('%s: %s failed:\n%s' % (name, os.path.basename(obj), r.stderr[-2000:]))
            done += 1
            if progress:
                progress(name, done, len(jobs))
    if os.path.exists(out):
        os.remove(out)
    subprocess.run(['ar', 'rcs', out] + [j[0] for j in jobs], check=True)
    with open(stamp, 'w') as f:
        f.write(key)
    return out


def main():
    for name in sys.argv[1:] or NAMES:
        print('built', build(name))


if __name__ == '__main__':
    main()
