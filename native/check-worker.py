#!/usr/bin/env python3
"""Exercise worker startup, protocol failure, and blocked pipe deadlines."""

import os
from pathlib import Path
import shutil
import signal
import subprocess

root = Path(__file__).resolve().parent.parent
fixture = root / "native/tmp/worker-fixture"
binary = fixture / "bin/gtts"
helper = fixture / "libexec/google-tts-local/gtts-engine"
pidfile = fixture / "worker.pid"
binary.parent.mkdir(parents=True, exist_ok=True)
helper.parent.mkdir(parents=True, exist_ok=True)
shutil.copy2(root / "native/build/gtts-native", binary)
helper.write_text('''#!/bin/sh
printf '%s\\n' "$$" > "$GTTS_FIXTURE_PIDFILE"
trap '' TERM
case "$GTTS_FIXTURE_MODE" in
  init-error) printf '%s\\n' '{"error":"initialization failed"}' ;;
  no-hello) ;;
  broken) printf '%s\\n' '{"speakers":{"sfg":"female"}}'
          IFS= read -r line
          printf '%s\\n' '{"error":"engine fault","broken":true}' ;;
  blocked-write) printf '%s\\n' '{"speakers":{"sfg":"female"}}' ;;
esac
while :; do :; done
''')
helper.chmod(0o755)
data = Path(os.environ.get("GTTS_DATA", str(Path.home() / "Library/Application Support/google-tts-local"))).resolve()
pack = data / "voices/en-us-x-multi.zvoice"
assert pack.is_file(), "Run gtts fetch --lang en-us before the worker check"
env = dict(os.environ, GTTS_DATA=str(data), GTTS_FIXTURE_PIDFILE=str(pidfile), GTTS_WORKER_TIMEOUT_MS="1500")
command = [str(binary), "speak", "--pack", str(pack), "--out", str(fixture / "unused.wav")]

for mode, expected, content in (
    ("init-error", "initialization failed", b"hello"),
    ("no-hello", "timed out", b"hello"),
    ("broken", "engine fault", b"hello"),
    ("blocked-write", "timed out", b"x" * (512 * 1024)),
):
    pidfile.unlink(missing_ok=True)
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               cwd=root, env=dict(env, GTTS_FIXTURE_MODE=mode), start_new_session=True)
    try:
        stdout, stderr = process.communicate(content, timeout=8)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.communicate()
        raise AssertionError(f"{mode}: CLI did not stop") from None
    assert process.returncode == 1 and expected.encode() in stderr, (mode, process.returncode, stdout, stderr)
    worker_pid = int(pidfile.read_text().strip())
    try:
        os.kill(worker_pid, 0)
    except ProcessLookupError:
        pass
    else:
        raise AssertionError(f"{mode}: worker {worker_pid} was not reaped")

print("Worker startup cleanup, broken-worker handling, and pipe deadlines: passed.")
