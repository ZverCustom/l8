#!/usr/bin/env python3
"""Upload the USB-only v3 diagnostic and save 20 seconds of serial output."""
import argparse
from pathlib import Path
import subprocess
import sys
import time

import serial
from serial.tools import list_ports


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="ESP32 port; auto-select if exactly one is connected")
    parser.add_argument("--capture-only", action="store_true", help="read an already flashed diagnostic")
    args = parser.parse_args()
    project = Path(__file__).resolve().parent
    devices = [p for p in list_ports.comports() if (p.vid, p.pid) == (0x303A, 0x1001)]
    if args.port:
        devices = [p for p in devices if p.device == args.port]
    if len(devices) != 1:
        parser.error("Expected one ESP32 USB JTAG/serial device; connect it or select --port.")
    device = devices[0]
    print(f"L8 v3 USB-only diagnostic: {device.device}, serial={device.serial_number}", flush=True)
    if not args.capture_only:
        print("Uploading diagnostic; this replaces the application on the selected board.", flush=True)
        subprocess.run(
            [sys.executable, "-m", "platformio", "run", "-d", str(project),
             "-t", "upload", "--upload-port", device.device],
            check=True,
        )
        time.sleep(2)

    # Re-discover by serial number: native USB may acquire a new device path.
    deadline = time.monotonic() + 15
    port = None
    while time.monotonic() < deadline:
        for candidate in list_ports.comports():
            if (candidate.vid, candidate.pid) != (0x303A, 0x1001):
                continue
            matches = (candidate.serial_number == device.serial_number
                       if device.serial_number else candidate.device == device.device)
            if not matches:
                continue
            connection = serial.Serial(baudrate=115200, timeout=0.25)
            connection.port = candidate.device
            connection.dtr = False
            connection.rts = False
            try:
                connection.open()
            except serial.SerialException:
                continue
            port = connection
            break
        if port is not None:
            break
        time.sleep(0.5)
    if port is None:
        raise RuntimeError("Cannot open ESP32 serial port after upload; check USB and port permissions.")

    log = project / ".pio" / "usb-bringup.log"
    log.parent.mkdir(exist_ok=True)
    print(f"Recording 20 seconds from {port.port} to {log}", flush=True)
    captured = bytearray()
    try:
        with log.open("wb") as output:
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                data = port.read(min(port.in_waiting or 1, 4096))
                if data:
                    captured.extend(data)
                    output.write(data)
                    output.flush()
                    sys.stdout.write(data.decode("utf-8", errors="replace"))
                    sys.stdout.flush()
    finally:
        port.close()
    if b"L8 v3 USB diagnostic" not in captured:
        raise RuntimeError(f"No v3 diagnostic report received; inspect {log}.")
    print(f"\nSaved: {log}")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError, serial.SerialException) as error:
        print(f"Error: {error}", file=sys.stderr)
        sys.exit(1)
