#!/usr/bin/env python3
"""
MattedComms - local Slack-style chat for AI agents working on Matted Engine.

Zero dependencies (Python 3.8+ stdlib + sqlite3). Server + client in one file.

Usage:
  python mattedcomms.py send [--user NAME] [--channel #general] "message text"
  python mattedcomms.py recv [--user NAME] [--channel #chan] [--follow] [--peek] [--all]
  python mattedcomms.py history [--channel #chan] [--limit 20]
  python mattedcomms.py channels | who | health
  python mattedcomms.py task add "do the thing" | task list | task done 3
  python mattedcomms.py serve          (normally auto-started)

Notes for agents:
  - `recv` prints only NEW messages since your last read and remembers the
    cursor per channel (state stored in .comms/state_<user>.json).
  - Always pass --user so presence and cursors work per agent.
  - Long-polling: `recv --follow` blocks and streams; plain `recv` waits a
    moment for new messages and returns (exit code 0 even if empty).
"""
import argparse
import json
import os
import socket
import sqlite3
import subprocess
import sys
import time
import urllib.parse
import urllib.request
import urllib.error
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DATA_DIR = ROOT / ".comms"
DB_PATH = DATA_DIR / "chat.db"
PORT = int(os.environ.get("MATTEDCOMMS_PORT", "8790"))
BASE = f"http://127.0.0.1:{PORT}"

DEFAULT_USER = os.environ.get("MATTED_USER") or f"agent-{socket.gethostname().lower().split('.')[0]}"

SCHEMA = """
CREATE TABLE IF NOT EXISTS messages (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  ts REAL NOT NULL,
  user TEXT NOT NULL,
  channel TEXT NOT NULL,
  text TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_msg_chan ON messages(channel, id);
CREATE TABLE IF NOT EXISTS presence (
  user TEXT PRIMARY KEY,
  last_seen REAL NOT NULL
);
CREATE TABLE IF NOT EXISTS tasks (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  created_ts REAL NOT NULL,
  text TEXT NOT NULL,
  status TEXT NOT NULL DEFAULT 'open',
  claimed_by TEXT,
  done_ts REAL
);
"""


# ---------------------------------------------------------------- server side
def db():
    conn = sqlite3.connect(str(DB_PATH), timeout=10)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA busy_timeout=10000")
    return conn


def norm_channel(ch):
    ch = (ch or "#general").strip()
    return ch if ch.startswith("#") else f"#{ch}"


def touch_presence(conn, user):
    conn.execute("INSERT INTO presence(user,last_seen) VALUES(?,?) "
                 "ON CONFLICT(user) DO UPDATE SET last_seen=excluded.last_seen",
                 (user, time.time()))
    conn.commit()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # keep the log file quiet
        pass

    # -- helpers ---------------------------------------------------------
    def send_json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def read_body(self):
        length = int(self.headers.get("Content-Length", "0") or 0)
        if length <= 0:
            return {}
        try:
            return json.loads(self.rfile.read(length).decode())
        except Exception:
            return {}

    # -- routes ----------------------------------------------------------
    def do_GET(self):
        path, _, query = self.path.partition("?")
        params = {}
        for part in query.split("&"):
            if "=" in part:
                k, _, v = part.partition("=")
                params[k] = urllib.parse.unquote(v)

        if path == "/health":
            return self.send_json({"ok": True, "service": "mattedcomms", "port": PORT})

        if path == "/latest":
            with db() as conn:
                row = conn.execute("SELECT COALESCE(MAX(id), 0) AS max_id FROM messages").fetchone()
            return self.send_json({"max_id": row["max_id"]})

        if path == "/channels":
            with db() as conn:
                rows = conn.execute("SELECT DISTINCT channel FROM messages ORDER BY channel").fetchall()
            return self.send_json({"channels": [r["channel"] for r in rows]})

        if path == "/who":
            with db() as conn:
                rows = conn.execute("SELECT user, last_seen FROM presence ORDER BY last_seen DESC").fetchall()
            now = time.time()
            users = [{"user": r["user"], "seconds_ago": int(now - r["last_seen"])} for r in rows]
            return self.send_json({"users": users})

        if path == "/history":
            chan = norm_channel(params.get("channel", "#general"))
            limit = int(params.get("limit", "50"))
            with db() as conn:
                rows = conn.execute(
                    "SELECT id,ts,user,channel,text FROM messages WHERE channel=? "
                    "ORDER BY id DESC LIMIT ?", (chan, limit)).fetchall()
            msgs = [dict(r) for r in reversed(rows)]
            return self.send_json({"channel": chan, "messages": msgs})

        if path == "/poll":
            chan = params.get("channel", "")
            after = int(params.get("after_id", "0"))
            wait = min(float(params.get("wait", "0")), 25.0)
            deadline = time.time() + wait
            while True:
                with db() as conn:
                    if chan:
                        rows = conn.execute(
                            "SELECT id,ts,user,channel,text FROM messages WHERE id>? AND channel=? "
                            "ORDER BY id ASC LIMIT 500", (after, norm_channel(chan))).fetchall()
                    else:
                        rows = conn.execute(
                            "SELECT id,ts,user,channel,text FROM messages WHERE id>? "
                            "ORDER BY id ASC LIMIT 500", (after,)).fetchall()
                msgs = [dict(r) for r in rows]
                if msgs or time.time() >= deadline:
                    return self.send_json({"messages": msgs, "after_id": after})
                time.sleep(0.25)

        return self.send_json({"error": f"unknown GET {path}"}, 404)

    def do_POST(self):
        path = self.path.split("?")[0]
        body = self.read_body()
        user = (body.get("user") or DEFAULT_USER).strip()

        if path == "/send":
            text = (body.get("text") or "").strip()
            if not text:
                return self.send_json({"error": "empty message"}, 400)
            chan = norm_channel(body.get("channel", "#general"))
            with db() as conn:
                cur = conn.execute(
                    "INSERT INTO messages(ts,user,channel,text) VALUES(?,?,?,?)",
                    (time.time(), user, chan, text))
                conn.commit()
                touch_presence(conn, user)
                mid = cur.lastrowid
            return self.send_json({"ok": True, "id": mid, "channel": chan})

        if path == "/presence":
            with db() as conn:
                touch_presence(conn, user)
            return self.send_json({"ok": True})

        if path == "/task":
            action = body.get("action", "list")
            with db() as conn:
                if action == "add":
                    text = (body.get("text") or "").strip()
                    if not text:
                        return self.send_json({"error": "empty task"}, 400)
                    cur = conn.execute(
                        "INSERT INTO tasks(created_ts,text,status,claimed_by) VALUES(?,?,'open',?)",
                        (time.time(), text, body.get("claimed_by")))
                    conn.commit()
                    return self.send_json({"ok": True, "id": cur.lastrowid})
                if action == "done":
                    conn.execute("UPDATE tasks SET status='done', done_ts=? WHERE id=?",
                                 (time.time(), int(body.get("id", 0))))
                    conn.commit()
                    return self.send_json({"ok": True})
                if action == "claim":
                    conn.execute("UPDATE tasks SET claimed_by=? WHERE id=?",
                                 (body.get("claimed_by", user), int(body.get("id", 0))))
                    conn.commit()
                    return self.send_json({"ok": True})
                rows = conn.execute("SELECT * FROM tasks ORDER BY id").fetchall()
            return self.send_json({"tasks": [dict(r) for r in rows]})

        return self.send_json({"error": f"unknown POST {path}"}, 404)


def run_server():
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    with db() as conn:
        conn.executescript(SCHEMA)
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    srv.daemon_threads = True
    print(f"[mattedcomms] listening on {BASE} (db: {DB_PATH})", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


# ---------------------------------------------------------------- client side
def http_json(method, path, payload=None, timeout=8):
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method)
    if data:
        req.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode())


def ensure_server():
    try:
        http_json("GET", "/health", timeout=1.0)
        return True
    except Exception:
        pass
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    logf = open(DATA_DIR / "server.log", "ab")
    kwargs = {"stdout": logf, "stderr": logf, "cwd": str(ROOT), "close_fds": True}
    if os.name == "nt":
        kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.DETACHED_PROCESS
    subprocess.Popen([sys.executable, str(Path(__file__).resolve()), "serve"], **kwargs)
    for _ in range(60):  # up to ~12s
        time.sleep(0.2)
        try:
            http_json("GET", "/health", timeout=1.0)
            return True
        except Exception:
            pass
    print("[mattedcomms] ERROR: could not start server; run: "
          f'python "{Path(__file__).resolve()}" serve', file=sys.stderr)
    return False


def fmt_msg(m):
    hhmmss = datetime.fromtimestamp(m["ts"]).strftime("%H:%M:%S")
    return f"[{m['id']:>5}] {hhmmss} {m['channel']:<12} {m['user']:<14} {m['text']}"


def load_cursor(user):
    p = DATA_DIR / f"state_{user}.json"
    if p.exists():
        try:
            return json.loads(p.read_text())
        except Exception:
            return {}
    return {}


def save_cursor(user, cur):
    p = DATA_DIR / f"state_{user}.json"
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(cur))


def cmd_recv(args):
    user = args.user
    state = load_cursor(user)
    follow = args.follow
    printed_any = False

    # presence heartbeat so `who` reflects readers, not only senders
    try:
        http_json("POST", "/presence", {"user": user}, timeout=2)
    except Exception:
        pass

    # Per-channel cursor when filtering (previously the global cursor was
    # advanced past other channels' messages, silently dropping them).
    # Falls back to the legacy global cursor so nothing is replayed.
    ckey = f"ch:{norm_channel(args.channel)}" if args.channel else "after_id"
    if ckey not in state and "after_id" in state:
        state[ckey] = state["after_id"]

    chan_q = f"&channel={urllib.parse.quote(norm_channel(args.channel))}" if args.channel else ""

    while True:
        after = state.get(ckey, 0)
        res = http_json("GET", f"/poll?after_id={after}{chan_q}&wait={3 if follow else 1.5}", timeout=40)
        msgs = res.get("messages", [])
        # /poll LIMIT(500) guard: drain until a short page so nothing is skipped
        while len(res.get("messages", [])) == 500:
            res = http_json("GET", f"/poll?after_id={msgs[-1]['id']}{chan_q}&wait=0", timeout=40)
            msgs += res.get("messages", [])
        if msgs:
            for m in msgs:  # server already filtered by channel; print everything
                print(fmt_msg(m))
            state[ckey] = msgs[-1]["id"]
            if not args.peek:
                save_cursor(user, state)
            printed_any = True
        if not follow:
            if not printed_any and not args.quiet:
                print(f"[mattedcomms] no new messages (cursor at {state.get(ckey, 0)})", file=sys.stderr)
            return 0
        time.sleep(0.3)


def cmd_unread(args):
    """Show per-channel unread counts without moving any cursor."""
    user = args.user
    state = load_cursor(user)
    channels = http_json("GET", "/channels")["channels"]
    total = 0
    for ch in channels:
        after = state.get(f"ch:{ch}", state.get("after_id", 0))
        r = http_json("GET", f"/poll?after_id={after}&channel={urllib.parse.quote(ch)}&wait=0")
        n = len(r["messages"])
        total += n
        if n:
            latest = r["messages"][-1]
            print(f"{ch:<12} {n:>4} unread  (latest #{latest['id']} from {latest['user']})")
    print(f"[mattedcomms] {total} unread across {len(channels)} channel(s)")


def cmd_status():
    now_users = http_json("GET", "/who")["users"]
    tasks = http_json("POST", "/task", {"action": "list"})["tasks"]
    print("== agents (alive = seen < 5 min) ==")
    for u in now_users:
        state = "ALIVE " if u["seconds_ago"] < 300 else "dark  "
        print(f"  [{state}] {u['user']:<20} seen {u['seconds_ago']}s ago")
    print("== tasks ==")
    done = sum(1 for t in tasks if t["status"] == "done")
    print(f"  {done}/{len(tasks)} done")
    for t in tasks:
        if t["status"] != "done":
            who = f" @{t['claimed_by']}" if t.get("claimed_by") else " (UNCLAIMED)"
            print(f"  #{t['id']:<3} {t['text']}{who}")


ONBOARD_DIGEST = """ONBOARDING DIGEST (full contract: AGENTS.md in repo root)
  1. You are a supporting agent. Default chat user = the name you were given
     (e.g. AgentC). ALWAYS pass --user <name> on every command.
  2. Check messages:  python tools\\mattedcomms\\mattedcomms.py recv --user <name>
     Then start your constant background watcher (REQUIRED, run in background):
       python tools\\mattedcomms\\mattedcomms.py watch --user <name>
  3. Claim before working:  task claim --id N    Mark finished:  task done --id N
  4. IDLE PROTOCOL: if no unclaimed task fits you, invent the next most valuable
     one, `task add "TASK-<next> [P0-P2][area] <what>. Gate: <done-check>"`,
     announce it in #tasks, claim it, and start. Never duplicate claimed work.
  5. Ownership (AGENTS.md): do not edit another agent's files; propose in #general.
  6. Build: scripts\\build.bat  (MSVC 2022 + CMake + Ninja, C++20, /O2)
  7. Break the build -> post in #build immediately.
"""


def cmd_join(user):
    http_json("POST", "/presence", {"user": user})
    http_json("POST", "/send", {"user": user, "channel": "#general",
                                "text": f"[onboard] {user} joined the project. Reading AGENTS.md, claiming work shortly."})
    print(ONBOARD_DIGEST)
    tasks = http_json("POST", "/task", {"action": "list"})["tasks"]
    unclaimed = [t for t in tasks if t["status"] != "done" and not t.get("claimed_by")]
    print("== unclaimed tasks ==")
    if not unclaimed:
        print("  (none - propose work in #general)")
    for t in unclaimed:
        print(f"  #{t['id']:<3} {t['text']}")
    print("== next steps ==")
    print("  1. task claim --id N --user <name>   (claim a task from the list)")
    print(f"  2. python tools\\mattedcomms\\mattedcomms.py watch --user {user}   (run in background)")
    print("  3. announce in #general what you are starting")


def cmd_watch(args):
    user = args.user
    if args.no_cursor:
        # cursor-free mode: safe to run alongside foreground recv for the same user
        after = int(http_json("GET", "/latest")["max_id"])
        state = None
    else:
        state = load_cursor(user)
        after = state.get("after_id", 0)
    last_status = time.time()
    print(f"[watch] {user} watching (poll + heartbeat every {args.interval}s, "
          f"status post every {args.status_every}s)", flush=True)
    while True:
        try:
            http_json("POST", "/presence", {"user": user}, timeout=5)
            res = http_json("GET", f"/poll?after_id={after}&wait={args.interval}", timeout=args.interval + 15)
            msgs = res.get("messages", [])
            if msgs:
                for m in msgs:
                    print(fmt_msg(m), flush=True)
                after = msgs[-1]["id"]
                if state is not None:
                    state["after_id"] = after
                    save_cursor(user, state)
            if args.status_every > 0 and time.time() - last_status >= args.status_every:
                http_json("POST", "/send", {"user": user, "channel": "#general",
                                            "text": f"[auto] {user} online, watching, awaiting tasks/messages."})
                last_status = time.time()
        except Exception as exc:  # server hiccup -> try to recover
            print(f"[watch] error: {exc}", file=sys.stderr)
            ensure_server()
            time.sleep(5)


def main():
    ap = argparse.ArgumentParser(prog="mattedcomms")
    sub = ap.add_subparsers(dest="cmd", required=True)

    def add_common(p):
        p.add_argument("--user", default=DEFAULT_USER)

    p = sub.add_parser("send")
    add_common(p)
    p.add_argument("--channel", default="#general")
    p.add_argument("text", nargs="+")

    p = sub.add_parser("recv")
    add_common(p)
    p.add_argument("--channel", default=None)
    p.add_argument("--follow", action="store_true")
    p.add_argument("--peek", action="store_true", help="do not advance read cursor")
    p.add_argument("--quiet", action="store_true")
    p.add_argument("--all", action="store_true", help="(accepted, default scans all channels)")

    p = sub.add_parser("history")
    p.add_argument("--channel", default="#general")
    p.add_argument("--limit", type=int, default=50)

    p = sub.add_parser("unread")
    add_common(p)

    sub.add_parser("channels")
    sub.add_parser("who")
    sub.add_parser("health")

    p = sub.add_parser("task")
    p.add_argument("action", choices=["add", "list", "done", "claim"])
    p.add_argument("text", nargs="*", default=[])
    p.add_argument("--id", type=int, default=0)
    p.add_argument("--unclaimed", action="store_true",
                   help="add to the board WITHOUT claiming it (stockpile/backlog)")
    add_common(p)

    p = sub.add_parser("join")
    add_common(p)

    p = sub.add_parser("watch")
    add_common(p)
    p.add_argument("--interval", type=int, default=60, help="poll window seconds")
    p.add_argument("--status-every", type=int, default=300, help="seconds between auto status posts (0=off)")
    p.add_argument("--no-cursor", action="store_true",
                   help="do not read/write the per-user cursor (safe alongside foreground recv)")

    sub.add_parser("status")
    sub.add_parser("serve")

    args = ap.parse_args()

    if args.cmd == "serve":
        run_server()
        return 0

    if not ensure_server():
        return 1

    if args.cmd == "send":
        text = " ".join(args.text)
        if text == "-":
            text = sys.stdin.read().strip()
        r = http_json("POST", "/send", {"user": args.user, "channel": args.channel, "text": text})
        print(f"[mattedcomms] sent #{r['id']} to {r['channel']}")
    elif args.cmd == "recv":
        return cmd_recv(args)
    elif args.cmd == "history":
        r = http_json("GET", f"/history?channel={urllib.parse.quote(args.channel)}&limit={args.limit}")
        for m in r["messages"]:
            print(fmt_msg(m))
    elif args.cmd == "unread":
        return cmd_unread(args)
    elif args.cmd == "channels":
        print("\n".join(http_json("GET", "/channels")["channels"]))
    elif args.cmd == "who":
        for u in http_json("GET", "/who")["users"]:
            print(f"{u['user']:<20} seen {u['seconds_ago']}s ago")
    elif args.cmd == "health":
        print(json.dumps(http_json("GET", "/health")))
    elif args.cmd == "join":
        cmd_join(args.user)
    elif args.cmd == "watch":
        cmd_watch(args)
    elif args.cmd == "status":
        cmd_status()
    elif args.cmd == "task":
        if args.action == "add":
            text = " ".join(args.text).strip()
            payload = {"action": "add", "text": text}
            if not args.unclaimed:
                payload["claimed_by"] = args.user
            r = http_json("POST", "/task", payload)
            print(f"[mattedcomms] task #{r['id']} added" +
                  ("" if args.unclaimed else f" and claimed by {args.user}"))
        elif args.action == "done":
            http_json("POST", "/task", {"action": "done", "id": args.id})
            print(f"[mattedcomms] task #{args.id} done")
        elif args.action == "claim":
            http_json("POST", "/task", {"action": "claim", "id": args.id, "claimed_by": args.user})
            print(f"[mattedcomms] task #{args.id} claimed by {args.user}")
        else:
            for t in http_json("POST", "/task", {"action": "list"})["tasks"]:
                mark = "x" if t["status"] == "done" else " "
                who = f" @{t['claimed_by']}" if t.get("claimed_by") else ""
                print(f"[{mark}] #{t['id']:<3} {t['text']}{who}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
