#!/usr/bin/env python3
"""isaaclua remote relay: runs on a server the console can reach (a VPS), Python 3.7+, no packages.

    sudo python3 relay.py --key <secret> [--port 80]

One port serves both sides:
  * the console (debug build with rom:/isaaclua_remote.ini) connects and says hello with the key;
  * HTTP for the person testing, every request with ?key=<secret>:
      GET  /status            {"connected": true, "version": "...", ...}
      POST /run?timeout=60    body = Lua; answer = what it printed and returned (text)
      GET  /last              the last answer (when /run timed out before it came)
  * the console's background module (isaac-remote sysmodule, hello version "sys-...") connects the same way and
    has its own endpoints: GET /sys/status, POST /sys/run (body = a command: help, ps, kill, relaunch, press,
    regs, peek, ls, get, put...), GET /sys/last. It stays connected when the game hangs or crashes.
Only one chunk runs at a time per side. Nothing is stored on disk.
"""
import argparse, asyncio, hmac, json, time, urllib.parse

class State:
    def __init__(self):
        self.writer = None      # the console's connection
        self.version = None
        self.since = None
        self.last_seen = None
        self.next_id = 1
        self.waiting = {}       # id -> Future
        self.last = ""
        self.busy = False

GAME, SYS = State(), State()
S = GAME
KEY = ""

def frame(t, payload: bytes) -> bytes:
    return b"%s %d\n" % (t.encode(), len(payload)) + payload

async def console(reader, writer, first_line: bytes):
    # first frame: "H <len>\n<key> <version>"
    try:
        n = int(first_line.split()[1])
        hello = (await reader.readexactly(n)).decode("utf-8", "replace")
    except Exception:
        writer.close(); return
    key, _, version = hello.partition(" ")
    if not hmac.compare_digest(key, KEY):
        writer.close(); return
    S = SYS if version.startswith("sys-") else GAME
    if S.writer:
        try: S.writer.close()
        except Exception: pass
    # a reconnect ends whatever the old connection was doing: free the slot and answer its waiters
    S.busy = False
    for f in S.waiting.values():
        if not f.done():
            f.set_result("ERROR the console reconnected")
    S.waiting.clear()
    S.writer, S.version, S.since, S.last_seen = writer, version, time.time(), time.time()
    print(time.strftime("%H:%M:%S"), "console connected:", version, flush=True)
    try:
        while True:
            line = await asyncio.wait_for(reader.readline(), 40)
            if not line:
                break
            t, n = line.decode().split()
            payload = await asyncio.wait_for(reader.readexactly(int(n)), 120)  # a stalled reply must not hold the slot forever
            S.last_seen = time.time()
            if t == "R":
                cid, _, text = payload.decode("utf-8", "replace").partition("\n")
                S.last = text
                S.busy = False
                fut = S.waiting.pop(cid, None)
                if fut and not fut.done():
                    fut.set_result(text)
            elif t == "P":
                writer.write(frame("P", b""))
                await writer.drain()
    except Exception as e:
        print(time.strftime("%H:%M:%S"), "console connection ended:", repr(e), flush=True)
    finally:
        if S.writer is writer:
            S.writer = None
            S.busy = False
            for f in S.waiting.values():
                if not f.done():
                    f.set_result("ERROR the console disconnected")
            S.waiting.clear()
        writer.close()

async def http(reader, writer, first_line: bytes):
    S = GAME
    try:
        method, target, _ = first_line.decode("latin-1").split(" ", 2)
    except ValueError:
        writer.close(); return
    headers = {}
    while True:
        h = await reader.readline()
        if h in (b"\r\n", b"\n", b""):
            break
        k, _, v = h.decode("latin-1").partition(":")
        headers[k.strip().lower()] = v.strip()
    body = b""
    if "content-length" in headers:
        body = await reader.readexactly(min(int(headers["content-length"]), 1 << 20))
    url = urllib.parse.urlsplit(target)
    q = dict(urllib.parse.parse_qsl(url.query))
    path = url.path
    if path.startswith("/sys/"):
        S, path = SYS, path[4:]
    status, out, ctype = 200, "", "text/plain; charset=utf-8"
    if not hmac.compare_digest(q.get("key", headers.get("x-key", "")), KEY):
        status, out = 403, "bad key"
    elif path == "/status":
        ctype = "application/json"
        out = json.dumps({"connected": S.writer is not None, "version": S.version, "busy": S.busy,
                          "since": S.since and round(time.time() - S.since),
                          "last_seen": S.last_seen and round(time.time() - S.last_seen)})
    elif path == "/last":
        out = S.last
    elif path == "/run" and method == "POST":
        if not S.writer:
            status, out = 503, "console not connected"
        elif S.busy:
            status, out = 409, "a chunk is still running (GET /last later)"
        else:
            cid = str(S.next_id); S.next_id += 1
            fut = asyncio.get_running_loop().create_future()
            S.waiting[cid] = fut
            S.busy = True
            S.writer.write(frame("C", cid.encode() + b"\n" + body))
            await S.writer.drain()
            try:
                out = await asyncio.wait_for(asyncio.shield(fut), float(q.get("timeout", 60)))
            except asyncio.TimeoutError:
                status, out = 504, "no answer yet (still running?): GET /last later"
    else:
        status, out = 404, "unknown"
    data = out.encode("utf-8")
    writer.write(b"HTTP/1.1 %d X\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n"
                 % (status, ctype.encode(), len(data)) + data)
    try:
        await writer.drain()
    finally:
        writer.close()

async def handle(reader, writer):
    try:
        first = await asyncio.wait_for(reader.readline(), 15)
    except Exception:
        writer.close(); return
    if first.startswith(b"H "):
        await console(reader, writer, first)
    else:
        await http(reader, writer, first)

async def main():
    global KEY
    ap = argparse.ArgumentParser()
    ap.add_argument("--key", required=True)
    ap.add_argument("--port", type=int, default=80)
    a = ap.parse_args()
    KEY = a.key
    srv = await asyncio.start_server(handle, "0.0.0.0", a.port)
    print("isaaclua relay on port", a.port, flush=True)
    async with srv:
        await srv.serve_forever()

if __name__ == "__main__":
    asyncio.run(main())
