#!/usr/bin/env python3
"""Parse (compile without running) every .lua file under the given directories with a LuaJIT
executable and report the files that fail.

  python3 tools/lua_parse_corpus.py [--luajit PATH] [-v] DIR [DIR ...]

The default interpreter is build/third_party/luajit-src/src/luajit; build it with
  MACOSX_DEPLOYMENT_TARGET=12.0 make -C build/third_party/luajit-src/src BUILDMODE=static \
       CC=clang XCFLAGS=-DLUAJIT_ENABLE_LUA52COMPAT luajit
after `python3 tools/thirdparty.py luajit`.
Exit status is 1 when any file fails to parse.
"""
import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# One LuaJIT process: reads file names from stdin, loadfile()s each (parse + compile only) and
# prints "FAIL\t<name>\t<error>" for every failure.
DRIVER = r'''
for name in io.stdin:lines() do
  local f, err = loadfile(name)
  if not f then io.write('FAIL\t', name, '\t', (tostring(err):gsub('\n', ' ')), '\n') end
end
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--luajit', default=os.path.join(ROOT, 'build', 'third_party', 'luajit-src', 'src', 'luajit'))
    ap.add_argument('-v', '--verbose', action='store_true', help='print the error message for each failure')
    ap.add_argument('dirs', nargs='+')
    a = ap.parse_args()
    if not os.path.isfile(a.luajit):
        sys.exit('no LuaJIT executable at %s (see --help for how to build it)' % a.luajit)

    files = []
    for d in a.dirs:
        for dp, dn, fn in os.walk(d):
            dn.sort()
            files += [os.path.join(dp, f) for f in sorted(fn) if f.lower().endswith('.lua')]
    r = subprocess.run([a.luajit, '-e', DRIVER], input='\n'.join(files) + '\n',
                       capture_output=True, text=True, errors='replace')
    if r.returncode:
        sys.exit('luajit driver failed:\n' + r.stderr)
    fails = [l.split('\t', 2) for l in r.stdout.splitlines() if l.startswith('FAIL\t')]
    for _, name, err in fails:
        print(('%s\n    %s' % (name, err)) if a.verbose else name)
    print('%d files, %d parsed, %d failed' % (len(files), len(files) - len(fails), len(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
