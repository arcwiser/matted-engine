#!/usr/bin/env python3
"""Map repository paths to AGENTS.md owners and detect cross-owner edits."""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path, PurePosixPath


# Keep in sync with AGENTS.md.
OWNERSHIP = {
    "AgentA": (
        "engine/core/**",
        "engine/render/render_queue.*",
        "engine/render/software/**",
        "engine/physics/**",
        "apps/**",
        "CMakeLists.txt",
        "scripts/**",
    ),
    "AgentB": (
        "engine/render/d3d12/**",
        "engine/render/vulkan/**",
        "engine/assets/**",
        "engine/physics/gjk/**",
        "engine/physics/fracture_voronoi.*",
        "tests/**",
    ),
    "AgentC": ("docs/**",),
    "shared": (
        "AGENTS.md",
        "BACKLOG.md",
        "ONBOARDING.md",
        "ONBOARD_PROMPT.txt",
        "README.md",
        "tools/**",
    ),
}


def normalize_path(raw: str) -> str:
    path = raw.replace("\\", "/")
    try:
        candidate = Path(raw).resolve()
        root = Path(__file__).resolve().parents[2]
        path = candidate.relative_to(root).as_posix()
    except (OSError, ValueError):
        path = path.removeprefix("./")
    return path.strip("/")


def matches(path: str, pattern: str) -> bool:
    pure = PurePosixPath(path)
    if pattern.endswith("/**"):
        prefix = pattern[:-3].rstrip("/")
        return path == prefix or path.startswith(prefix + "/")
    return pure.match(pattern)


def owner_for(raw_path: str) -> str:
    path = normalize_path(raw_path)
    # Specific nested ownership wins over broader ownership. This matters for
    # AgentB's GJK/Voronoi files nested inside AgentA's physics tree.
    for owner in ("AgentB", "AgentC", "AgentA", "shared"):
        if any(matches(path, pattern) for pattern in OWNERSHIP[owner]):
            return owner
    return "unknown"


def check_paths(user: str, paths: list[str]) -> int:
    cross_owned = False
    for raw_path in paths:
        path = normalize_path(raw_path)
        owner = owner_for(raw_path)
        if owner not in (user, "shared", "unknown"):
            cross_owned = True
            print(
                f"DENY {path}: owned by {owner}, current agent is {user}; "
                "coordinate in #general and wait for an ack",
                file=sys.stderr,
            )
        else:
            print(f"OK   {path}: {owner}")
    return 1 if cross_owned else 0


def selftest() -> int:
    cases = {
        "engine/core/log.cpp": "AgentA",
        "engine/render/render_queue.h": "AgentA",
        "engine/render/software/software_backend.cpp": "AgentA",
        "engine/physics/world.cpp": "AgentA",
        "apps/demo/main.cpp": "AgentA",
        "engine/render/d3d12/d3d12_backend.cpp": "AgentB",
        "engine/render/vulkan/backend.cpp": "AgentB",
        "engine/assets/gltf.cpp": "AgentB",
        "engine/physics/gjk/gjk.cpp": "AgentB",
        "engine/physics/fracture_voronoi.cpp": "AgentB",
        "tests/test_math.cpp": "AgentB",
        "docs/architecture.md": "AgentC",
        "tools/comms/daily_digest.py": "shared",
        "AGENTS.md": "shared",
        "misc/experimental.txt": "unknown",
    }
    failures = []
    for path, expected in cases.items():
        actual = owner_for(path)
        if actual != expected:
            failures.append(f"{path}: expected {expected}, got {actual}")

    if check_paths("AgentA", ["engine/core/log.cpp", "tests/test_math.cpp"]) != 1:
        failures.append("cross-owner check did not return exit 1")
    if check_paths("AgentB", ["tests/test_math.cpp", "tools/x.py", "misc/x"]) != 0:
        failures.append("own/shared/unknown check did not return exit 0")

    if failures:
        print("SELFTEST FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    print(f"SELFTEST OK: {len(cases)} ownership cases + exit-code checks")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", metavar="PATH")
    parser.add_argument(
        "--user",
        default=os.environ.get("MATTED_USER"),
        help="agent requesting the check (default: MATTED_USER)",
    )
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)
    if args.selftest:
        return selftest()
    if not args.user:
        parser.error("pass --user AGENT or set MATTED_USER")
    if not args.paths:
        parser.error("provide at least one path")
    return check_paths(args.user, args.paths)


if __name__ == "__main__":
    raise SystemExit(main())
