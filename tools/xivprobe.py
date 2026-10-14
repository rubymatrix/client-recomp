#!/usr/bin/env python3
"""Client for xivprobe, the recompiled client's test endpoint (runtime/portable/xiv_probe.c).

Launch the client with XIV_PROBE=<port> (127.0.0.1 only), then:

    tools/xivprobe.py state                      # player, zone, event, talk, menu, cutscene
    tools/xivprobe.py events [--since N] [--type lua,net] [--follow]
    tools/xivprobe.py lua                        # Lua binding call counts
    tools/xivprobe.py trace all|default|none|+_name,-_name [--raw 0|1]
    tools/xivprobe.py act interact actor=44D8000E
    tools/xivprobe.py act choose index=2
    tools/xivprobe.py wait-log "obtained" --timeout 30

As a module: Probe(port).state(), .events(since), .act(op, **kw), .wait_for(pred, types, timeout),
.wait_log(text), .wait_event(name=..., function=...). Port from XIV_PROBE (default 7979).
"""
import argparse
import json
import os
import re
import sys
import time
import urllib.parse
import urllib.request

DEFAULT_PORT = int(os.environ.get("XIV_PROBE") or 7979)


class ProbeError(Exception):
    pass


class Probe:
    def __init__(self, port=None, host="127.0.0.1"):
        self.base = "http://%s:%d" % (host, port or DEFAULT_PORT)
        self.cursor = 0  # events seen up to here (wait_* advance it)

    def _req(self, path, method="GET", timeout=10, **q):
        q = {k: v for k, v in q.items() if v is not None}
        url = self.base + path + ("?" + urllib.parse.urlencode(q) if q else "")
        req = urllib.request.Request(url, method=method, data=b"" if method == "POST" else None)
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return json.loads(r.read().decode("utf-8", "replace"))
        except OSError as e:
            raise ProbeError("%s: %s" % (url, e))

    def alive(self):
        try:
            return bool(self._req("/", timeout=2).get("xivprobe"))
        except ProbeError:
            return False

    def state(self):
        return self._req("/state")

    def events(self, since=0, types=None, wait_ms=0, limit=500):
        return self._req("/events", since=since, type=types, wait=wait_ms or None, limit=limit,
                         timeout=10 + wait_ms / 1000.0)

    def lua_counts(self):
        return self._req("/lua")

    def trace(self, lua=None, raw=None):
        return self._req("/trace", method="POST", lua=lua, raw=raw)

    def act(self, op, **kw):
        r = self._req("/act", method="POST", op=op, **kw)
        if not r.get("ok"):
            raise ProbeError("act %s: %s" % (op, r.get("error") or r))
        return r

    def mark(self):
        """Start watching from now: later wait_* calls only see newer events."""
        self.cursor = self.state()["seq"]
        return self.cursor

    def wait_for(self, pred, types=None, timeout=30, since=None):
        """The first event (after the cursor, or `since`) for which pred(event) is true; advances the cursor."""
        cur = self.cursor if since is None else since
        end = time.time() + timeout
        while True:
            left = end - time.time()
            if left <= 0:
                raise ProbeError("timed out after %ss" % timeout)
            r = self.events(cur, types, wait_ms=int(min(left, 5) * 1000))
            for e in r["events"]:
                if pred(e):
                    self.cursor = e["seq"]
                    return e
            cur = r["next"]
            self.cursor = cur

    def wait_log(self, text=None, timeout=30, regex=False, text_id=None):
        """A log/chat line: the server's SendMessage text or GameMessage text id, the client's
        _appendLogPool lines, or a message-window line (_appendMessagePool) by text id."""
        rx = re.compile(text if regex else re.escape(text), re.I) if text else None

        def match(e):
            if text_id is not None:
                if e.get("name") == "GameMessage" and e.get("text_id") == text_id:
                    return True
                if e.get("what") == "message" and (e.get("message") or {}).get("text_id") == text_id:
                    return True
            if rx is not None:
                if e.get("name") == "SendMessage" and rx.search(e.get("text", "")):
                    return True
                if e.get("what") == "log" and rx.search(json.dumps(e.get("args"), ensure_ascii=False)):
                    return True
            return False
        return self.wait_for(match, "net,state", timeout)

    def wait_event(self, name=None, function=None, op=None, timeout=30, **fields):
        """A world-connection message: name=EventStart|EventUpdate|RunEventFunction|EndEvent|KickEvent|
        GameMessage|SetMap|ZoneInComplete|SetTarget..., function= the RunEventFunction function."""
        def match(e):
            if name and e.get("name") != name:
                return False
            if function and e.get("function") != function:
                return False
            if op and e.get("op", "").lower() != ("%04x" % op if isinstance(op, int) else op.lower().replace("0x", "").zfill(4)):
                return False
            return all(e.get(k) == v for k, v in fields.items())
        return self.wait_for(match, "net", timeout)

    def wait_lua(self, name, timeout=30, pred=None):
        return self.wait_for(lambda e: e.get("name") == name and (pred is None or pred(e)), "lua", timeout)

    def wait_state(self, pred, timeout=30, every=0.25):
        end = time.time() + timeout
        while True:
            s = self.state()
            if pred(s):
                return s
            if time.time() > end:
                raise ProbeError("state condition not met in %ss" % timeout)
            time.sleep(every)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("state")
    e = sub.add_parser("events")
    e.add_argument("--since", type=int, default=0)
    e.add_argument("--type")
    e.add_argument("--limit", type=int, default=200)
    e.add_argument("--follow", "-f", action="store_true")
    sub.add_parser("lua")
    t = sub.add_parser("trace")
    t.add_argument("lua", nargs="?")
    t.add_argument("--raw", type=int)
    a = sub.add_parser("act")
    a.add_argument("op")
    a.add_argument("kv", nargs="*", help="key=value")
    w = sub.add_parser("wait-log")
    w.add_argument("text")
    w.add_argument("--timeout", type=float, default=30)
    a2 = ap.parse_args()
    p = Probe(a2.port)
    out = lambda o: print(json.dumps(o, indent=1, ensure_ascii=False))
    try:
        if a2.cmd == "state":
            out(p.state())
        elif a2.cmd == "events":
            since = a2.since
            while True:
                r = p.events(since, a2.type, wait_ms=5000 if a2.follow else 0, limit=a2.limit)
                for ev in r["events"]:
                    print(json.dumps(ev, ensure_ascii=False))
                since = r["next"]
                if not a2.follow:
                    print("# next=%d" % since, file=sys.stderr)
                    break
        elif a2.cmd == "lua":
            for b in sorted(p.lua_counts()["bindings"], key=lambda b: -b["calls"]):
                print("%8d %s %s.%s" % (b["calls"], "*" if b["traced"] else " ", b["cls"], b["name"]))
        elif a2.cmd == "trace":
            out(p.trace(a2.lua, a2.raw))
        elif a2.cmd == "act":
            out(p.act(a2.op, **dict(kv.split("=", 1) for kv in a2.kv)))
        elif a2.cmd == "wait-log":
            p.mark()
            out(p.wait_log(a2.text, a2.timeout))
    except ProbeError as ex:
        print("xivprobe: %s" % ex, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
