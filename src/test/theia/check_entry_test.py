#!/usr/bin/env python3
"""Exercise both Bootloader validation and the low-to-linked alias switch."""
import struct
import sys

sys.dont_write_bytecode = True
from check_entry import check_v5f_entry

good = (0x0040006f, 0x30047073, 0x080160b7, 0x01208067)
check_v5f_entry(struct.pack('<IIII', *good))
for bad in (
    (0x30047073, *good[1:]),  # BL rejects CSR as the first instruction
    (0x0060006f, *good[1:]),  # relative jump leaves execution in low alias
    (good[0], 0x00000013, *good[2:]),  # interrupts were not masked
    (*good[:2], 0x000160b7, good[3]),  # absolute target is still low alias
    (*good[:3], 0x00000013),  # no jump to linked address
):
    try:
        check_v5f_entry(struct.pack('<IIII', *bad))
    except AssertionError:
        continue
    raise AssertionError('invalid V5F entry accepted')
print('H415 V5F entry: BL jump, IRQ masking and linked address switch PASS')
