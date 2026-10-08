#!/usr/bin/env python3
"""
MSP + DFU flashing helper for Betaflight targets.

Typical flow:
1) Talk MSP over serial to verify FC is alive.
2) Send MSP_REBOOT (mode=bootloader ROM).
3) Wait for DFU USB device (default 0483:df11).
4) Flash with dfu-util.
"""

from __future__ import annotations

import argparse
import csv
import os
import shutil
import subprocess
import sys
import time
from typing import List, Tuple

import serial


MSP_API_VERSION = 1
MSP_FC_VARIANT = 2
MSP_FC_VERSION = 3
MSP_REBOOT = 68
MSP_ATTITUDE = 108

MSP_REBOOT_FIRMWARE = 0
MSP_REBOOT_BOOTLOADER_ROM = 1


def _msp_v1_frame(cmd: int, payload: bytes = b"") -> bytes:
    if not (0 <= cmd <= 255):
        raise ValueError(f"MSP v1 command out of range: {cmd}")
    if len(payload) > 255:
        raise ValueError(f"MSP v1 payload too large: {len(payload)}")

    size = len(payload)
    checksum = size ^ cmd
    for b in payload:
        checksum ^= b

    return b"$M<" + bytes((size, cmd)) + payload + bytes((checksum,))


def _read_exact(port: serial.Serial, size: int, deadline: float) -> bytes:
    data = bytearray()
    while len(data) < size:
        if time.time() > deadline:
            raise TimeoutError("Timed out while reading MSP frame")
        chunk = port.read(size - len(data))
        if not chunk:
            continue
        data.extend(chunk)
    return bytes(data)


def _read_msp_v1_frame(port: serial.Serial, timeout_s: float) -> Tuple[str, int, bytes]:
    deadline = time.time() + timeout_s

    while True:
        if time.time() > deadline:
            raise TimeoutError("Timed out waiting for MSP response header")

        b = port.read(1)
        if not b or b != b"$":
            continue

        head = _read_exact(port, 2, deadline)
        if head[0] != ord("M"):
            continue
        direction = chr(head[1])  # '>' response, '!' error
        if direction not in (">", "!"):
            continue

        hdr = _read_exact(port, 2, deadline)
        size = hdr[0]
        cmd = hdr[1]
        payload = _read_exact(port, size, deadline) if size else b""
        checksum = _read_exact(port, 1, deadline)[0]

        calc = size ^ cmd
        for x in payload:
            calc ^= x
        if calc != checksum:
            # Corrupt frame, keep scanning stream.
            continue

        return direction, cmd, payload


def msp_request(port: serial.Serial, cmd: int, payload: bytes = b"", timeout_s: float = 1.0) -> bytes:
    frame = _msp_v1_frame(cmd, payload)
    port.write(frame)
    port.flush()

    while True:
        direction, resp_cmd, resp_payload = _read_msp_v1_frame(port, timeout_s=timeout_s)
        if resp_cmd != (cmd & 0xFF):
            # Ignore unrelated frame and continue.
            continue
        if direction == "!":
            raise RuntimeError(f"MSP command {cmd} rejected by FC")
        return resp_payload


def open_serial(port_name: str, baud: int, timeout_s: float) -> serial.Serial:
    return serial.Serial(
        port=port_name,
        baudrate=baud,
        timeout=min(timeout_s, 0.2),
        write_timeout=timeout_s,
        exclusive=True,
        dsrdtr=True,
        rtscts=False,
    )


def _read_until(port: serial.Serial, marker: bytes, timeout_s: float) -> bytes:
    deadline = time.time() + timeout_s
    data = bytearray()
    while time.time() < deadline:
        chunk = port.read(256)
        if chunk:
            data.extend(chunk)
            if marker in data:
                return bytes(data)
            # CLI prompt is usually "\r\n# " at end.
            if data.endswith(b"# "):
                return bytes(data)
    raise TimeoutError(f"Timed out waiting for marker {marker!r}")


def cli_enter(port: serial.Serial, timeout_s: float) -> None:
    # Send '#' to switch from MSP to CLI mode.
    port.write(b"#")
    port.flush()
    _read_until(port, b"# ", timeout_s)


def cli_exec(port: serial.Serial, command: str, timeout_s: float) -> str:
    port.write(command.encode("ascii", errors="ignore") + b"\r\n")
    port.flush()
    out = _read_until(port, b"\r\n# ", timeout_s)
    return out.decode(errors="replace")


def cli_exit_noreboot(port: serial.Serial, timeout_s: float) -> None:
    try:
        cli_exec(port, "exit noreboot", timeout_s)
    except Exception:
        # Do not fail the whole script on CLI exit path.
        pass


def has_usb_device(vidpid: str) -> bool:
    try:
        out = subprocess.check_output(["lsusb"], text=True, stderr=subprocess.STDOUT)
    except Exception:
        return False
    return vidpid.lower() in out.lower()


def wait_for_dfu(vidpid: str, timeout_s: float) -> None:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if has_usb_device(vidpid):
            return
        time.sleep(0.2)
    raise TimeoutError(f"DFU device {vidpid} not detected within {timeout_s:.1f}s")


def require_dfu_util() -> str:
    exe = shutil.which("dfu-util")
    if not exe:
        raise RuntimeError("dfu-util not found in PATH")
    return exe


def cmd_ping(args: argparse.Namespace) -> int:
    with open_serial(args.port, args.baud, args.timeout) as port:
        port.reset_input_buffer()

        api = msp_request(port, MSP_API_VERSION, timeout_s=args.timeout)
        variant = msp_request(port, MSP_FC_VARIANT, timeout_s=args.timeout)
        version = msp_request(port, MSP_FC_VERSION, timeout_s=args.timeout)

    api_str = ".".join(str(x) for x in api[:3]) if len(api) >= 3 else api.hex()
    variant_str = variant.decode(errors="replace")
    version_str = ".".join(str(x) for x in version[:3]) if len(version) >= 3 else version.hex()

    print(f"MSP API: {api_str}")
    print(f"FC variant: {variant_str}")
    print(f"FC version: {version_str}")
    return 0


def cmd_bootloader(args: argparse.Namespace) -> int:
    with open_serial(args.port, args.baud, args.timeout) as port:
        port.reset_input_buffer()
        try:
            msp_request(
                port,
                MSP_REBOOT,
                payload=bytes((MSP_REBOOT_BOOTLOADER_ROM,)),
                timeout_s=args.timeout,
            )
            print("MSP reboot command acknowledged")
        except TimeoutError:
            # Normal if the port drops quickly during reset.
            print("MSP reboot ACK timeout (likely reset in progress), continuing")

    wait_for_dfu(args.dfu_vidpid, args.wait_dfu)
    print(f"DFU detected: {args.dfu_vidpid}")
    return 0


def cmd_flash(args: argparse.Namespace) -> int:
    dfu_util = require_dfu_util()
    if not os.path.isfile(args.firmware):
        raise FileNotFoundError(f"Firmware file not found: {args.firmware}")

    if not args.skip_msp_bootloader:
        if not args.port:
            raise ValueError("--port is required unless --skip-msp-bootloader is used")
        boot_args = argparse.Namespace(
            port=args.port,
            baud=args.baud,
            timeout=args.timeout,
            dfu_vidpid=args.dfu_vidpid,
            wait_dfu=args.wait_dfu,
        )
        cmd_bootloader(boot_args)
    else:
        wait_for_dfu(args.dfu_vidpid, args.wait_dfu)
        print(f"DFU detected: {args.dfu_vidpid}")

    cmd = [dfu_util, "-a", str(args.dfu_alt)]
    if args.firmware.lower().endswith(".dfu"):
        cmd += ["-D", args.firmware, "-s", ":leave"]
    else:
        cmd += ["-s", f"{args.dfu_addr}:leave", "-D", args.firmware]

    print("Running:", " ".join(cmd))
    subprocess.run(cmd, check=True)
    print("Flash completed")
    return 0


def cmd_cli(args: argparse.Namespace) -> int:
    commands: List[str] = args.command or []
    if not commands:
        raise ValueError("At least one --command is required")

    with open_serial(args.port, args.baud, args.timeout) as port:
        port.reset_input_buffer()
        port.reset_output_buffer()
        cli_enter(port, args.timeout)

        for cmd in commands:
            print(f"$ {cmd}")
            print(cli_exec(port, cmd, args.timeout), end="")

        if not args.stay_in_cli:
            cli_exit_noreboot(port, args.timeout)

    return 0


def cmd_bfdebug(args: argparse.Namespace) -> int:
    commands: List[str] = []
    if args.commands:
        commands = list(args.commands)
    else:
        commands = ["status", "imudebug"]
        if args.include_tasks:
            commands.append("tasks")
        if args.include_gyroregisters:
            commands.append("gyroregisters")

    repeats = max(1, int(args.repeat))
    interval_s = max(0.0, float(args.interval))

    with open_serial(args.port, args.baud, args.timeout) as port:
        port.reset_input_buffer()
        port.reset_output_buffer()
        cli_enter(port, args.timeout)

        for i in range(repeats):
            print(f"===== BFDEBUG {i + 1}/{repeats} =====")
            for cmd in commands:
                print(f"$ {cmd}")
                print(cli_exec(port, cmd, args.timeout), end="")
            if i + 1 < repeats and interval_s > 0:
                time.sleep(interval_s)

        if not args.stay_in_cli:
            cli_exit_noreboot(port, args.timeout)

    return 0


def cmd_attlog(args: argparse.Namespace) -> int:
    duration_s = max(0.1, float(args.duration))
    interval_s = max(0.0, float(args.interval))

    rows = []
    with open_serial(args.port, args.baud, args.timeout) as port:
        port.reset_input_buffer()
        port.reset_output_buffer()

        start = time.time()
        while True:
            now = time.time()
            if (now - start) >= duration_s:
                break

            payload = msp_request(port, MSP_ATTITUDE, timeout_s=args.timeout)
            if len(payload) >= 6:
                # MSP_ATTITUDE: int16 roll[0.1deg], pitch[0.1deg], yaw[deg]
                roll_raw = int.from_bytes(payload[0:2], "little", signed=True)
                pitch_raw = int.from_bytes(payload[2:4], "little", signed=True)
                yaw_raw = int.from_bytes(payload[4:6], "little", signed=True)
                rows.append((now - start, roll_raw / 10.0, pitch_raw / 10.0, float(yaw_raw)))

            if interval_s > 0:
                time.sleep(interval_s)

    if not rows:
        raise RuntimeError("No MSP_ATTITUDE samples collected")

    if args.out_csv:
        with open(args.out_csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["t_s", "roll_deg", "pitch_deg", "yaw_deg"])
            w.writerows(rows)
        print(f"CSV saved: {args.out_csv}")

    total_dt = rows[-1][0] - rows[0][0] if len(rows) > 1 else 0.0
    sample_rate = ((len(rows) - 1) / total_dt) if total_dt > 0 else 0.0
    roll_vals = [r[1] for r in rows]
    pitch_vals = [r[2] for r in rows]
    yaw_vals = [r[3] for r in rows]

    print(f"samples: {len(rows)}")
    print(f"sample_rate_hz: {sample_rate:.2f}")
    print(f"roll_min_max: {min(roll_vals):.2f} {max(roll_vals):.2f}")
    print(f"pitch_min_max: {min(pitch_vals):.2f} {max(pitch_vals):.2f}")
    print(f"yaw_min_max: {min(yaw_vals):.2f} {max(yaw_vals):.2f}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="MSP + DFU + CLI utility for Betaflight")
    sub = p.add_subparsers(dest="cmd", required=True)

    def add_serial_common(sp: argparse.ArgumentParser, required_port: bool = True) -> None:
        sp.add_argument("--port", required=required_port, help="MSP serial port, e.g. /dev/ttyACM0")
        sp.add_argument("--baud", type=int, default=115200, help="MSP baud rate")
        sp.add_argument("--timeout", type=float, default=1.0, help="MSP timeout seconds")

    def add_dfu_common(sp: argparse.ArgumentParser) -> None:
        sp.add_argument("--dfu-vidpid", default="0483:df11", help="DFU USB VID:PID")
        sp.add_argument("--wait-dfu", type=float, default=15.0, help="DFU wait timeout seconds")

    p_ping = sub.add_parser("ping", help="Query MSP API/variant/version")
    add_serial_common(p_ping)
    p_ping.set_defaults(func=cmd_ping)

    p_boot = sub.add_parser("bootloader", help="Send MSP reboot to ROM bootloader and wait DFU")
    add_serial_common(p_boot)
    add_dfu_common(p_boot)
    p_boot.set_defaults(func=cmd_bootloader)

    p_flash = sub.add_parser("flash", help="MSP reboot to DFU then flash using dfu-util")
    add_serial_common(p_flash, required_port=False)
    add_dfu_common(p_flash)
    p_flash.add_argument("--firmware", required=True, help="Firmware file path for dfu-util -D")
    p_flash.add_argument("--dfu-alt", type=int, default=0, help="dfu-util -a value")
    p_flash.add_argument("--dfu-addr", default="0x08000000", help="dfu-util target address")
    p_flash.add_argument(
        "--skip-msp-bootloader",
        action="store_true",
        help="Do not send MSP reboot; only wait DFU then flash",
    )
    p_flash.set_defaults(func=cmd_flash)

    p_cli = sub.add_parser("cli", help="Run one or more Betaflight CLI commands")
    add_serial_common(p_cli)
    p_cli.add_argument(
        "--command",
        action="append",
        help="CLI command to run (repeat for multiple commands)",
    )
    p_cli.add_argument(
        "--stay-in-cli",
        action="store_true",
        help="Do not send 'exit noreboot' at the end",
    )
    p_cli.set_defaults(func=cmd_cli)

    p_debug = sub.add_parser("bfdebug", help="Collect Betaflight debug info via CLI")
    add_serial_common(p_debug)
    p_debug.add_argument(
        "--commands",
        nargs="+",
        help="Custom CLI command list (default: status imudebug [+tasks])",
    )
    p_debug.add_argument("--repeat", type=int, default=1, help="How many snapshots to collect")
    p_debug.add_argument("--interval", type=float, default=0.5, help="Seconds between snapshots")
    p_debug.add_argument("--include-tasks", action="store_true", help="Include 'tasks' output")
    p_debug.add_argument(
        "--include-gyroregisters",
        action="store_true",
        help="Include 'gyroregisters' output",
    )
    p_debug.add_argument(
        "--stay-in-cli",
        action="store_true",
        help="Do not send 'exit noreboot' at the end",
    )
    p_debug.set_defaults(func=cmd_bfdebug)

    p_att = sub.add_parser("attlog", help="Sample MSP_ATTITUDE and optionally save CSV")
    add_serial_common(p_att)
    p_att.add_argument("--duration", type=float, default=5.0, help="Log duration in seconds")
    p_att.add_argument("--interval", type=float, default=0.01, help="Delay between samples in seconds")
    p_att.add_argument("--out-csv", help="Output CSV path")
    p_att.set_defaults(func=cmd_attlog)

    return p


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    try:
        return int(args.func(args))
    except Exception as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
