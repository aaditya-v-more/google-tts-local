#!/usr/bin/env python3
"""Reject calls and external tail branches in code executed in the custom ABI."""
import re
import subprocess
import sys

objects = sys.argv[1:]
if len(objects) != 2:
    raise SystemExit("usage: check-fast.py allocator.o guest-fast.o")
for path in objects:
    output = subprocess.check_output(["xcrun", "llvm-objdump", "-dr", path], text=True)
    undefined = subprocess.check_output(["xcrun", "nm", "-u", path], text=True).split()
    permitted = {"_guest_native_current_key"} if path.endswith("guest_native_fast.o") else set()
    if set(undefined) - permitted:
        raise SystemExit(f"{path}: unexpected undefined symbols: {undefined}")
    for line in output.splitlines():
        instruction = re.match(r"\s*[0-9a-f]+:\s+[0-9a-f]{8}\s+([a-z0-9.]+)\s*(.*)", line)
        if instruction:
            op, operands = instruction.groups()
            if op.startswith("bl") or op in {"braa", "brab", "braaz", "brabz", "br"} or re.search(r"\b[wx]18\b", operands):
                raise SystemExit(f"{path}: forbidden instruction: {line}")
        if "ARM64_RELOC_BRANCH26" in line:
            target = line.split()[-1]
            if target not in {"_guest_native_heap_alloc", "_guest_native_heap_free"}:
                raise SystemExit(f"{path}: unexpected branch relocation: {line}")
    if "___chkstk_darwin" in output or "___stack_chk_fail" in output:
        raise SystemExit(f"{path}: unexpected compiler runtime call")
print("custom ABI fast-path disassembly: PASS")
