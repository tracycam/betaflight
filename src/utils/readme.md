# MSP + DFU Script Readme

Script path:
- `src/utils/msp_dfu_flash.py`

## Purpose
- Test MSP communication with the FC over serial.
- Reboot FC to ROM bootloader via MSP.
- Wait for DFU device and flash firmware with `dfu-util`.

## Prerequisites
- Python 3
- `pyserial` (`python3 -c "import serial"`)
- `dfu-util`
- FC USB device attached to WSL (via `usbipd` on Windows host).

## Commands

### 1) MSP communication test
```bash
python3 src/utils/msp_dfu_flash.py ping --port /dev/ttyACM0 --baud 115200
```

### 2) Run one or more CLI commands
```bash
python3 src/utils/msp_dfu_flash.py cli \
  --port /dev/ttyACM0 \
  --command "imudebug" \
  --command "status"
```

### 3) Collect BF debug snapshot(s)
```bash
python3 src/utils/msp_dfu_flash.py bfdebug \
  --port /dev/ttyACM0 \
  --repeat 3 \
  --interval 0.5 \
  --include-tasks
```

### 4) Log attitude samples (for SFLP vs Mahony A/B compare)
```bash
python3 src/utils/msp_dfu_flash.py attlog \
  --port /dev/ttyACM0 \
  --duration 10 \
  --interval 0.01 \
  --out-csv /tmp/attitude_sflp.csv
```

### 5) Reboot to bootloader (MSP -> DFU)
```bash
python3 src/utils/msp_dfu_flash.py bootloader --port /dev/ttyACM0
```

### 6) One-shot flash (normal firmware mode -> MSP reboot -> DFU flash)
```bash
python3 src/utils/msp_dfu_flash.py flash \
  --port /dev/ttyACM0 \
  --firmware obj/betaflight_2025.12.1_STM32F405_SPEEDYBEEF405WING.bin
```

### 7) Flash when FC is already in DFU mode
```bash
python3 src/utils/msp_dfu_flash.py flash \
  --skip-msp-bootloader \
  --firmware obj/betaflight_2025.12.1_STM32F405_SPEEDYBEEF405WING.bin
```

## Firmware file notes
- `.bin` is flashed with `-s 0x08000000:leave`.
- `.dfu` is flashed with `-s :leave`.

## Build reminder
- Build command policy is in:
  - `AGENTS.md`
