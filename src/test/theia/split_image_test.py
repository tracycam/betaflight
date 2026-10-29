#!/usr/bin/env python3
"""Config preservation and malformed-image checks for H415 updates."""
import importlib.util
import sys
from pathlib import Path

sys.dont_write_bytecode = True
path = Path(__file__).resolve().parents[2] / "utils/ch32h415_split_image.py"
spec = importlib.util.spec_from_file_location("split_image", path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

config = bytes((i * 13) & 255 for i in range(module.CONFIG_END - module.CONFIG_START))
before = bytearray([0xff] * (module.END - module.BASE))
before[module.CONFIG_START-module.BASE:module.CONFIG_END-module.BASE] = config
image = bytes([0x42] * (module.CONFIG_START-module.BASE)) + bytes(len(config)) + bytes([0x91] * 1234)
parts = module.split_image(image)
assert [p[0] for p in parts] == [module.CONFIG_END, module.BASE]
for address, data, _ in parts:
    before[address-module.BASE:address-module.BASE+len(data)] = data
assert before[module.CONFIG_START-module.BASE:module.CONFIG_END-module.BASE] == config
assert before[:module.CONFIG_START-module.BASE] == image[:module.CONFIG_START-module.BASE]
assert before[module.CONFIG_END-module.BASE:module.CONFIG_END-module.BASE+1234] == image[module.CONFIG_END-module.BASE:]
for bad in (b"", image[:module.CONFIG_END-module.BASE],
            image[:module.CONFIG_START-module.BASE] + b"\x01" + image[module.CONFIG_START-module.BASE+1:],
            bytes(module.END-module.BASE+1)):
    try:
        module.split_image(bad)
    except ValueError:
        continue
    raise AssertionError("invalid image accepted")
print("H415 split image: preserve configuration, code first, reject invalid layouts PASS")
