#!/usr/bin/env python3
"""Synchronous faults must terminate the helper even with signals masked."""
import os
import subprocess
import sys

env = dict(os.environ)
env.pop("GTTS_NATIVE_BACKEND", None)
binary = sys.argv[1]
for mode in ("--fault-trap", "--fault-access", "--fault-thread"):
    try:
        result = subprocess.run([binary, mode], capture_output=True, text=True, env=env, timeout=10)
    except subprocess.TimeoutExpired:
        raise SystemExit(f"{mode}: guest fault did not terminate the worker") from None
    if result.returncode == 0 and "skipped (custom ABI unavailable)" in result.stdout:
        print("in-process fault checks: skipped (custom ABI unavailable)")
        break
    if result.returncode != 139 or "In-process engine fault" not in result.stderr:
        raise SystemExit(f"{mode}: unexpected result {result.returncode}: {result.stderr}")
else:
    print("in-process faults terminate main and guest worker threads: PASS")
