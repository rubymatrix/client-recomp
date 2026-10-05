#!/usr/bin/env python3
"""Waypoints for the scripted-input `walkto` from the Map Server's navmesh.

    route.py X Y Z [--player NAME] [--from X,Y,Z] [--tol 1.0]

Asks the running Map Server (admin API, METEOR_ADMIN_URL, default http://127.0.0.1:1990) for a
walkable path from the player's server position to X,Y,Z, using the zone's Detour navmesh
(Map Server/Navigation/ZoneNavMesh.cs). It prints `via` lines for the corners and a final
`walkto` (each with the point's height, so a switchback on a ramp is not cut short), ready to
append to a running timeline:

    ./xivctl input $(recomp/tools/input/route.py 57 4 -1205 | tr '\\n' '\\0' | xargs -0)   # or:
    recomp/tools/input/route.py 57 4 -1205 >> recomp/build/run/input.txt

Y matters: it picks the floor (a walkway over a road). Without a path (off the mesh, another
island) it exits 1 and prints nothing.
"""
import argparse
import json
import math
import os
import sys
import urllib.request

LUA = r"""
local nm = player.CurrentArea.navMesh
if nm == nil then return "nomesh" end
local p = player:GetPos()
local fx, fy, fz = %s
if fx == nil then fx, fy, fz = p[0], p[1], p[2] end
local ok1, s = nm:TrySnapToGround(fx, fy, fz)
local ok2, e = nm:TrySnapToGround(%f, %f, %f)
if not ok1 or not ok2 then return "offmesh" end
-- the player's own movement queue is the only List<Vector3> Lua can reach; it is filled and
-- emptied inside this call (under the zone lock), so the player never moves along it
local L = player.positionUpdates
L:Clear()
local ok = nm:TryFindPath(s, e, 1.0, 400, 0.0, L)
local out = {}
for i = 0, L.Count - 1 do local v = L[i]; out[#out+1] = string.format("%%.2f %%.2f %%.2f", v.X, v.Y, v.Z) end
L:Clear()
if not ok then return "nopath" end
return table.concat(out, ";")
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('x', type=float)
    ap.add_argument('y', type=float)
    ap.add_argument('z', type=float)
    ap.add_argument('--player', help='character name (default: the only one online)')
    ap.add_argument('--from', dest='frm', help='start X,Y,Z instead of the player position')
    ap.add_argument('--tol', type=float, default=1.0, help='final walkto tolerance')
    ap.add_argument('--timeout', type=int, default=30000, help='ms per leg')
    ap.add_argument('--walkpath', action='store_true', help='print one `walkpath` line instead')
    a = ap.parse_args()

    frm = 'nil, nil, nil'
    if a.frm:
        fx, fy, fz = (float(v) for v in a.frm.split(','))
        frm = '%f, %f, %f' % (fx, fy, fz)
    body = {'code': LUA % (frm, a.x, a.y, a.z), 'depth': 1}
    if a.player:
        body['player'] = a.player
    base = os.environ.get('METEOR_ADMIN_URL', 'http://127.0.0.1:1990').rstrip('/')
    req = urllib.request.Request(base + '/lua', data=json.dumps(body).encode(),
                                 headers={'Content-Type': 'application/json'})
    res = json.loads(urllib.request.urlopen(req, timeout=10).read())
    out = res.get('result')
    if not isinstance(out, str) or ';' not in out and out in ('nomesh', 'offmesh', 'nopath', ''):
        print('route: %s' % (out or res), file=sys.stderr)
        sys.exit(1)
    pts = [tuple(map(float, p.split())) for p in out.split(';') if p]

    # keep the corners: drop points the path runs straight through. A sharp corner (a switchback
    # on a ramp) gets a tight tolerance so the walk does not cut it and hit the ramp's side.
    keep = [(pts[0], 0.0)]
    for i in range(1, len(pts) - 1):
        ax, az = pts[i][0] - keep[-1][0][0], pts[i][2] - keep[-1][0][2]
        bx, bz = pts[i + 1][0] - pts[i][0], pts[i + 1][2] - pts[i][2]
        turn = abs(math.atan2(ax * bz - az * bx, ax * bx + az * bz))
        if turn > math.radians(12) or math.hypot(ax, az) > 20:
            keep.append((pts[i], turn))
    keep.append((pts[-1], 0.0))
    lines = []
    for p, turn in keep[1:-1]:
        tol = 0.6 if turn > math.radians(50) else 1.0 if turn > math.radians(25) else 1.5
        lines.append('+0 via %.2f %.2f %.1f %d %.2f' % (p[0], p[2], tol, a.timeout, p[1]))
    end = keep[-1][0]
    lines.append('+0 walkto %.2f %.2f %.2f %d %.2f' % (end[0], end[2], a.tol, a.timeout, end[1]))
    if a.walkpath:
        print('+0 walkpath ' + ' '.join('%.2f %.2f' % (p[0], p[2]) for p, _ in keep[1:]))
    else:
        print('\n'.join(lines))


if __name__ == '__main__':
    main()
