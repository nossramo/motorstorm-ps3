#!/usr/bin/env python3
"""Restore the pinned open-source runtime and apply our saved source changes."""
import argparse
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SDK = ROOT / "vendor/ps3recomp"
PATCH = ROOT / "patches/ps3recomp-local.patch"
REMOTE = "https://github.com/sp00nznet/ps3recomp.git"


def git(*args, cwd=SDK, check=True):
    return subprocess.run(["git", *args], cwd=cwd, check=check,
                          text=True, capture_output=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Read-only local verification")
    args = parser.parse_args()
    commit = (ROOT / "research/ps3recomp-commit.txt").read_text().strip()
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise SystemExit("Invalid pinned runtime commit")
    if "GIT binary patch" in PATCH.read_text():
        raise SystemExit("Runtime backup must contain only source changes")
    if not SDK.exists():
        if args.check:
            raise SystemExit("Runtime is absent; run without --check to restore it")
        SDK.mkdir(parents=True)
        git("init")
        git("remote", "add", "origin", REMOTE)
        git("fetch", "--depth=1", "origin", commit)
        git("checkout", "--detach", "FETCH_HEAD")
    if git("rev-parse", "HEAD").stdout.strip() != commit:
        raise SystemExit("Existing runtime has a different HEAD; left unchanged")
    if git("apply", "--reverse", "--check", str(PATCH), check=False).returncode == 0:
        print("Pinned runtime and saved patch verified.")
        return
    if args.check:
        raise SystemExit("Saved patch does not match this runtime checkout")
    if git("status", "--porcelain", "--untracked-files=all").stdout.strip():
        raise SystemExit("Runtime has other changes; left unchanged")
    git("apply", "--check", str(PATCH))
    git("apply", str(PATCH))
    print("Pinned runtime restored with local source changes.")


if __name__ == "__main__":
    main()
