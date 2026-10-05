#!/usr/bin/env python3
"""A walking route for the scripted input from the Map Server's navmesh.

    route.py X Y Z [--player NAME] [--from X,Y,Z] [--tol 0.8] [--via | --walkpath]

Asks the running Map Server (admin API, METEOR_ADMIN_URL, default http://127.0.0.1:1990) for a
walkable path from the player's server position to X,Y,Z on the zone's Detour navmesh
(Map Server/Navigation/ZoneNavMesh.cs, built with agent radius 0.5: the raw path runs 0.5 from
every wall it turns round). The path comes back as a point every metre; each point is then
pushed away from nearby walls (rays on the navmesh in 16 directions, aiming for CLEARANCE metres
on each side, never more than PUSH_MAX, and only where the moved point stays in line of sight
of the original), and away from every NPC and object of the zone (and of the merged seamless
zone) standing near it (NPC_CLEAR): the client collides with them, the navmesh does not have
them. The same goes for the spots in obstacles.json next to this script: things the client
collides with that the navmesh lets through (found by `follow` stalling there; its log line names
the spot). So the walk keeps off the walls and out of the crowd instead of grazing every corner.

Default output, for a running timeline (recomp/docs/scripted-input.md, `follow`):

    +0 path clear
    +0 path X Z Y X Z Y ...      (20 points per line)
    +0 follow TOL TIMEOUT

    recomp/tools/input/route.py 57 4 -1205 >> recomp/build/run/input.txt
    ./xivctl input "$(recomp/tools/input/route.py 57 4 -1205)"   # same thing, one argument per line

--via prints the older corner-to-corner `via`/`walkto` lines; --walkpath one `walkpath` line.
Y picks the floor (a walkway over a road). Without a path (off the mesh, another island) it
exits 1 and prints nothing.
"""
import argparse
import json
import math
import os
import sys
import urllib.error
import urllib.request

CLEARANCE = 1.1   # wanted distance from a wall, metres (the navmesh already keeps 0.5)
PUSH_MAX = 0.6    # how far a point may move off the wall
OBSTACLES = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'obstacles.json')
NPC_CLEAR = 1.4   # distance kept from NPCs and objects: the navmesh does not have them, the client does

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
local ok = nm:TryFindPath(s, e, 1.0, 1000, 0.0, L)
local pts = {}
for i = 0, L.Count - 1 do local v = L[i]; pts[#pts+1] = {v.X, v.Y, v.Z} end
L:Clear()
if not ok then return "nopath" end
table.insert(pts, 1, {s.X, s.Y, s.Z})

-- push away from walls: clearance along 16 rays (binary search on line of sight)
local CLR, PMAX, NCLR = %f, %f, %f
local a, b = nm:TrySnapToGround(s.X, s.Y, s.Z)   -- two scratch vectors
local c, d = nm:TrySnapToGround(s.X, s.Y, s.Z)
local from, to = b, d
local function clear(x, y, z, dx, dz)
  from.X, from.Y, from.Z = x, y, z
  to.Y = y
  to.X, to.Z = x + dx * CLR, z + dz * CLR
  if nm:HasLineOfSight(from, to) then return CLR end
  local lo, hi = 0.0, CLR
  for _ = 1, 4 do
    local m = (lo + hi) / 2
    to.X, to.Z = x + dx * m, z + dz * m
    if nm:HasLineOfSight(from, to) then lo = m else hi = m end
  end
  return lo
end
local push = {}
for i, q in ipairs(pts) do
  local px, pz = 0, 0
  if i > 1 and i < #pts then
    for k = 0, 15 do
      local ang = k * math.pi / 8
      local dx, dz = math.sin(ang), math.cos(ang)
      local cl = clear(q[1], q[2], q[3], dx, dz)
      px, pz = px - dx * (CLR - cl), pz - dz * (CLR - cl)
    end
    local len = math.sqrt(px * px + pz * pz)
    if len > 0 then
      local m = math.min(PMAX, len * 0.5)
      px, pz = px / len * m, pz / len * m
    end
  end
  push[i] = {px, pz}
end
-- NPCs and objects stand on the navmesh but block the client (they have collision): push the
-- path to NPC_CLEAR from each one standing near it on the same floor
local obst = { %s }
local function collect(area)
  if area == nil then return end
  local A = area:GetAllActors()
  for i = 0, A.Count - 1 do
    local o = A[i]
    -- tostring is "<Class> [0xID]": players do not block each other
    if string.sub(tostring(o), 1, 7) ~= "Player " then obst[#obst+1] = {o.positionX, o.positionY, o.positionZ} end
  end
end
collect(player.CurrentArea)
pcall(function() collect(player.zone2) end)
-- smooth the pushes along the path (two passes of 1-2-1), so the route bends, not zigzags
for _ = 1, 2 do
  local sm = {}
  for i = 1, #push do
    if i == 1 or i == #push then sm[i] = push[i] else
      sm[i] = {(push[i-1][1] + 2 * push[i][1] + push[i+1][1]) / 4, (push[i-1][2] + 2 * push[i][2] + push[i+1][2]) / 4}
    end
  end
  push = sm
end
local out = {}
for i, q in ipairs(pts) do
  local x, y, z = q[1], q[2], q[3]
  local nx, nz = x + push[i][1], z + push[i][2]
  if math.abs(push[i][1]) + math.abs(push[i][2]) > 0.02 then
    from.X, from.Y, from.Z = x, y, z
    to.X, to.Y, to.Z = nx, y, nz
    if nm:HasLineOfSight(from, to) then x, z = nx, nz end
  end
  -- then out to NCLR from NPCs and known obstacles (after smoothing, which would halve it)
  if i > 1 and i < #pts then
    for _ = 1, 3 do
      for _, o in ipairs(obst) do
        local dx, dz = x - o[1], z - o[3]
        local d = math.sqrt(dx * dx + dz * dz)
        if d < NCLR and math.abs(y - o[2]) < 2.0 then
          if d < 0.05 then dx, dz, d = 1, 0, 1 end
          local nx2, nz2 = o[1] + dx / d * NCLR, o[3] + dz / d * NCLR
          from.X, from.Y, from.Z = x, y, z
          to.X, to.Y, to.Z = nx2, y, nz2
          if nm:HasLineOfSight(from, to) then x, z = nx2, nz2 end
        end
      end
    end
  end
  out[#out+1] = string.format("%%.2f %%.2f %%.2f", x, y, z)
end
return table.concat(out, ";")
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('x', type=float)
    ap.add_argument('y', type=float)
    ap.add_argument('z', type=float)
    ap.add_argument('--player', help='character name (default: the only one online)')
    ap.add_argument('--from', dest='frm', help='start X,Y,Z instead of the player position')
    ap.add_argument('--tol', type=float, default=0.8, help='how close to the end counts as arrived')
    ap.add_argument('--timeout', type=int, default=0, help='ms for the whole walk (default: by length)')
    ap.add_argument('--clearance', type=float, default=CLEARANCE)
    ap.add_argument('--push', type=float, default=PUSH_MAX, help='max wall push (0 = raw navmesh path)')
    ap.add_argument('--npc-clear', type=float, default=NPC_CLEAR, help='distance kept from NPCs and objects (0 = ignore them)')
    g = ap.add_mutually_exclusive_group()
    g.add_argument('--via', action='store_true', help='corner waypoints as via/walkto lines (old)')
    g.add_argument('--walkpath', action='store_true', help='corner waypoints as one walkpath line (old)')
    a = ap.parse_args()
    if a.via or a.walkpath:
        a.push = a.npc_clear = 0.0   # corner waypoints come from the raw path (pushed corners drift off ramps)

    frm = 'nil, nil, nil'
    if a.frm:
        fx, fy, fz = (float(v) for v in a.frm.split(','))
        frm = '%f, %f, %f' % (fx, fy, fz)
    extra = ''
    if a.npc_clear > 0:
        try:
            with open(OBSTACLES) as f:
                extra = ', '.join('{%f, %f, %f}' % (o['x'], o['y'], o['z']) for o in json.load(f)['obstacles'])
        except FileNotFoundError:
            pass
    body = {'code': LUA % (frm, a.x, a.y, a.z, a.clearance, a.push, a.npc_clear, extra), 'depth': 1}
    if a.player:
        body['player'] = a.player
    base = os.environ.get('METEOR_ADMIN_URL', 'http://127.0.0.1:1990').rstrip('/')
    req = urllib.request.Request(base + '/lua', data=json.dumps(body).encode(),
                                 headers={'Content-Type': 'application/json'})
    try:
        res = json.loads(urllib.request.urlopen(req, timeout=30).read())
    except urllib.error.HTTPError as e:
        print('route: admin API %s: %s' % (e.code, e.read().decode(errors='replace')[:300]), file=sys.stderr)
        sys.exit(1)
    out = res.get('result')
    if not isinstance(out, str) or ';' not in out:
        print('route: %s' % (out or res), file=sys.stderr)
        sys.exit(1)
    pts = [tuple(map(float, p.split())) for p in out.split(';') if p]
    length = sum(math.hypot(pts[i][0] - pts[i - 1][0], pts[i][2] - pts[i - 1][2]) for i in range(1, len(pts)))

    if not (a.via or a.walkpath):
        timeout = a.timeout or int(15000 + length * 600)   # ~4.5 m/s running, with room for slow corners
        print('+0 path clear')
        for i in range(0, len(pts), 20):
            print('+0 path ' + ' '.join('%.2f %.2f %.2f' % (p[0], p[2], p[1]) for p in pts[i:i + 20]))
        print('+0 follow %.2f %d' % (a.tol, timeout))
        return

    # corners only (the older via/walkpath output)
    keep = [(pts[0], 0.0)]
    for i in range(1, len(pts) - 1):
        ax, az = pts[i][0] - keep[-1][0][0], pts[i][2] - keep[-1][0][2]
        bx, bz = pts[i + 1][0] - pts[i][0], pts[i + 1][2] - pts[i][2]
        turn = abs(math.atan2(ax * bz - az * bx, ax * bx + az * bz))
        if turn > math.radians(12) or math.hypot(ax, az) > 20:
            keep.append((pts[i], turn))
    keep.append((pts[-1], 0.0))
    if a.walkpath:
        print('+0 walkpath ' + ' '.join('%.2f %.2f' % (p[0], p[2]) for p, _ in keep[1:]))
        return
    t = a.timeout or 30000
    for p, turn in keep[1:-1]:
        tol = 0.6 if turn > math.radians(50) else 1.0 if turn > math.radians(25) else 1.5
        print('+0 via %.2f %.2f %.1f %d %.2f' % (p[0], p[2], tol, t, p[1]))
    end = keep[-1][0]
    print('+0 walkto %.2f %.2f %.2f %d %.2f' % (end[0], end[2], a.tol, t, end[1]))


if __name__ == '__main__':
    main()
