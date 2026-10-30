# CH32H41x third-party sources

## Provenance

- WCH CH32H417 CMSIS and standard peripheral library. The device header identifies version 1.0.0, dated 2025-03-01, with copyright held by Nanjing Qinheng Microelectronics Co., Ltd.
- Initial Betaflight integration obtained from `openwch/betaflight` PR #15364 and the related BF_H417 implementation.
- CherryUSB middleware identifies itself as version 1.5.0 and carries Apache-2.0 notices.

These sources are vendored to make the CH32H415 build reproducible. They are not relicensed as Betaflight GPL code.

## Local changes

The Betaflight integration includes targeted corrections for DMA register-width handling and ownership, flash lock/status/timeout and interrupt-state restoration, ADC sample mode selection, USB endpoint barriers, speed reporting and session reset behavior. Keep any future changes narrowly scoped and document the corresponding hardware reason.

Do not reformat these third-party files as part of Betaflight style cleanups. First-party code under `src/platform/CH32` follows the Betaflight coding style instead.

## License review

WCH header notices permit use of the software, modified or not, on WCH microcontrollers and must be preserved. They do not constitute a complete project-wide license review. Confirm WCH redistribution terms with maintainers before upstream submission. CherryUSB remains under Apache-2.0.

## Toolchain

The CH32 target uses a WCH RISC-V GCC 12.2.0 toolchain because fast interrupt prologues require the `_xw` extension. The download rule enables TLS certificate verification and pins the archive SHA256. See `src/platform/CH32/mk/tools.mk` for the source URL and digest.
