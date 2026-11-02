#!/usr/bin/env python3
"""Print a read-only daily digest from the local MattedComms database."""

from __future__ import annotations

import argparse
import re
import sqlite3
import sys
from collections import Counter
from datetime import date, datetime, time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_DB = ROOT / ".comms" / "chat.db"
MENTION_RE = re.compile(r"(?<![\w@])@([A-Za-z0-9_-]+)")


def parse_since(value: str) -> float:
    try:
        day = date.fromisoformat(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("expected YYYY-MM-DD") from exc
    # MattedComms stores Unix timestamps generated in local time. A naive local
    # midnight converted with timestamp() therefore matches its display rules.
    return datetime.combine(day, time.min).timestamp()


def open_read_only(path: Path) -> sqlite3.Connection:
    if not path.is_file():
        raise FileNotFoundError(f"MattedComms database not found: {path}")
    uri = path.resolve().as_uri() + "?mode=ro"
    conn = sqlite3.connect(uri, uri=True, timeout=5)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA query_only=ON")
    return conn


def fmt_timestamp(timestamp: float | None) -> str:
    if timestamp is None:
        return "-"
    return datetime.fromtimestamp(timestamp).astimezone().isoformat(timespec="seconds")


def build_digest(conn: sqlite3.Connection, since: float) -> str:
    messages = conn.execute(
        "SELECT id, ts, user, channel, text FROM messages "
        "WHERE ts >= ? ORDER BY id",
        (since,),
    ).fetchall()
    tasks = conn.execute(
        "SELECT id, created_ts, text, status, claimed_by, done_ts "
        "FROM tasks ORDER BY id"
    ).fetchall()

    known_agents = {
        row["user"]
        for row in conn.execute("SELECT user FROM presence")
    }
    known_agents.update(row["user"] for row in messages)
    known_agents.update(row["claimed_by"] for row in tasks if row["claimed_by"])
    canonical_agent = {agent.casefold(): agent for agent in known_agents}

    channel_counts = Counter(row["channel"] for row in messages)
    mentions: Counter[str] = Counter()
    task_announcements = []
    for row in messages:
        for raw_mention in MENTION_RE.findall(row["text"]):
            agent = canonical_agent.get(raw_mention.casefold())
            if agent:
                mentions[agent] += 1
        if row["channel"] == "#tasks":
            task_announcements.append(row)

    lines = [
        "MattedComms daily digest",
        f"Since: {fmt_timestamp(since)}",
        f"Messages: {len(messages)}",
        "",
        "Per-channel messages:",
    ]
    if channel_counts:
        width = max(len(channel) for channel in channel_counts)
        lines.extend(
            f"  {channel:<{width}}  {channel_counts[channel]:>5}"
            for channel in sorted(channel_counts)
        )
    else:
        lines.append("  (none)")

    lines.extend(["", "@mentions:"])
    if mentions:
        width = max(len(agent) for agent in mentions)
        lines.extend(
            f"  @{agent:<{width}}  {mentions[agent]:>5}"
            for agent in sorted(mentions, key=str.casefold)
        )
    else:
        lines.append("  (none)")

    lines.extend(["", "Task announcements in #tasks:"])
    if task_announcements:
        for row in task_announcements:
            when = datetime.fromtimestamp(row["ts"]).strftime("%Y-%m-%d %H:%M:%S")
            lines.append(f"  [{when}] {row['user']}: {row['text']}")
    else:
        lines.append("  (none)")

    open_count = sum(row["status"] != "done" for row in tasks)
    done_count = len(tasks) - open_count
    lines.extend(
        [
            "",
            f"Current task snapshot: {open_count} open, {done_count} done",
        ]
    )
    for row in tasks:
        owner = f" @{row['claimed_by']}" if row["claimed_by"] else " (unclaimed)"
        mark = "x" if row["status"] == "done" else " "
        lines.append(f"  [{mark}] #{row['id']} {row['text']}{owner}")

    lines.extend(
        [
            "",
            "Limitation: the tasks table stores only its current snapshot, not an",
            "audit history. Task changes above are inferred from #tasks messages;",
            "unannounced claims/completions cannot be reconstructed historically.",
        ]
    )
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--since",
        required=True,
        type=parse_since,
        metavar="YYYY-MM-DD",
        help="include messages from local midnight on this date",
    )
    parser.add_argument(
        "--db",
        type=Path,
        default=DEFAULT_DB,
        help=argparse.SUPPRESS,
    )
    args = parser.parse_args(argv)
    try:
        with open_read_only(args.db) as conn:
            print(build_digest(conn, args.since))
    except (FileNotFoundError, sqlite3.Error) as exc:
        print(f"daily_digest: error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
