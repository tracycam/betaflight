#!/usr/bin/env python3
"""Check the actual BF ELF/bin against the internal H415 partition layout."""
import hashlib
import json
import pathlib
import struct
import sys
elf_path, bin_path = map(pathlib.Path, sys.argv[1:3])
elf, binary = elf_path.read_bytes(), bin_path.read_bytes()
assert elf[:6] == b'\x7fELF\x01\x01'
header = struct.unpack_from('<16sHHIIIIIHHHHHH', elf)
assert header[2] == 243 and header[4] == 0x08016000
phoff, phsize, phnum = header[5], header[9], header[10]
segments = []
for i in range(phnum):
    kind, offset, vma, lma, size, memory_size, flags, alignment = struct.unpack_from('<IIIIIIII', elf, phoff + i * phsize)
    if kind != 1 or not memory_size:
        continue
    assert not (vma < 0x20126000 and vma + memory_size > 0x20100000), hex(vma)
    if size:
        assert lma >= 0x08016000 and lma + size <= 0x080F0000, hex(lma)
        image_offset = lma - 0x08016000
        assert binary[image_offset:image_offset + size] == elf[offset:offset + size]
    segments.append({'vma': hex(vma), 'lma': hex(lma), 'bytes': size, 'memory_bytes': memory_size})
assert len(binary) <= 0x080F0000 - 0x08016000
print(json.dumps({'elf': str(elf_path), 'bin': str(bin_path), 'entry': hex(header[4]),
    'bytes': len(binary), 'sha256': hashlib.sha256(binary).hexdigest(), 'segments': segments}, indent=2))
