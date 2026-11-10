"""Validate the Bootloader-compatible H415 V5F entry trampoline."""
import struct


def check_v5f_entry(binary):
    # BL starts V5F through its low Flash alias and validates a forward JAL.
    # Interrupts must then be masked, and execution must switch to the linked
    # 0x080... alias before any PC-relative calculations of RAM addresses.
    entry, irq_off, load_target, jump_target = struct.unpack_from('<IIII', binary)
    assert entry == 0x0040006f  # jal x0, +4
    assert irq_off == 0x30047073  # csrci mstatus, 8
    assert load_target & 0xfff == 0x0b7  # lui ra, immediate
    assert jump_target & 0xfffff == 0x08067  # jalr x0, immediate(ra)
    low = jump_target >> 20
    if low & 0x800:
        low -= 0x1000
    target = ((load_target & 0xfffff000) + low) & 0xffffffff
    assert 0x08016000 < target < 0x08018000 and not target & 1
