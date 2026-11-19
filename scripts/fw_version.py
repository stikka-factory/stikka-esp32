#!/usr/bin/env python3
"""Firmware version: the VERSION file (semver) plus build metadata.

As a PlatformIO extra script (see platformio.ini) it defines FIRMWARE_VERSION
(e.g. "1.2.0+3f9c1ab", or "1.2.0+3f9c1ab.dirty" with uncommitted changes) and
FIRMWARE_ENV (the PlatformIO environment, which the server uses to pick the
matching image for remote updates).

On the command line:
  scripts/fw_version.py                 print the full version string
  scripts/fw_version.py --core          print just the VERSION file contents
  scripts/fw_version.py --bump patch    bump VERSION (patch | minor | major)
"""
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(globals().get("__file__", sys.argv[0])), ".."))
try:  # PlatformIO runs extra scripts with __file__ undefined and PROJECT_DIR set
    Import("env")  # type: ignore  # noqa: F821
    ROOT = env.subst("$PROJECT_DIR")  # type: ignore  # noqa: F821
    IN_PIO = True
except NameError:
    IN_PIO = False

SEMVER = re.compile(r"^\d+\.\d+\.\d+$")


def core() -> str:
    with open(os.path.join(ROOT, "VERSION")) as f:
        v = f.read().strip()
    if not SEMVER.match(v):
        raise SystemExit(f"VERSION must look like 1.2.3, got {v!r}")
    return v


def full() -> str:
    try:
        sha = subprocess.run(["git", "rev-parse", "--short=7", "HEAD"], cwd=ROOT, capture_output=True, text=True, check=True).stdout.strip()
        dirty = subprocess.run(["git", "status", "--porcelain", "--untracked-files=no"], cwd=ROOT, capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return core()
    return f"{core()}+{sha}" + (".dirty" if dirty else "")


def bump(part: str) -> str:
    major, minor, patch = (int(x) for x in core().split("."))
    if part == "major":
        major, minor, patch = major + 1, 0, 0
    elif part == "minor":
        minor, patch = minor + 1, 0
    else:
        patch += 1
    new = f"{major}.{minor}.{patch}"
    with open(os.path.join(ROOT, "VERSION"), "w") as f:
        f.write(new + "\n")
    return new


if IN_PIO:
    env.Append(CPPDEFINES=[  # type: ignore  # noqa: F821
        ("FIRMWARE_VERSION", env.StringifyMacro(full())),  # type: ignore  # noqa: F821
        ("FIRMWARE_ENV", env.StringifyMacro(env.subst("$PIOENV"))),  # type: ignore  # noqa: F821
    ])
elif __name__ == "__main__":
    args = sys.argv[1:]
    if args[:1] == ["--bump"] and len(args) == 2 and args[1] in ("patch", "minor", "major"):
        print(bump(args[1]))
    elif args == ["--core"]:
        print(core())
    elif not args:
        print(full())
    else:
        raise SystemExit(__doc__)
