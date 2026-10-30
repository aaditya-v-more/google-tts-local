#!/usr/bin/python3
"""Emit named aliases for the fixed native import transition bank."""
print('.text\n.p2align 2')
for identifier in range(1, 2049):
    print('.globl _gtts_h%04d\n_gtts_h%04d:\n    mov w16, #%d\n    b _native_import_entry' %
          (identifier, identifier, identifier))
