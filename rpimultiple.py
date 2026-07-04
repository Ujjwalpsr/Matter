#!/usr/bin/env python3
"""
Matter Bridge GPIO Wrapper
Launches chip-bridge-app and monitors GPIO state changes via log output.

Devices:
  "First Plug"  → GPIO 17
  "Second Plug" → GPIO 27
  "Third Plug"  → GPIO 22

Pairing code: 34970112332
Usage: python3 bridge_gpio.py
"""

import subprocess
import sys
import signal
import os

CHIP_BRIDGE_APP = "/home/admin/connectedhomeip/out/linux-arm64-bridge/chip-bridge-app"
DISCRIMINATOR   = "3840"
PASSCODE        = "20202021"
KVS_PATH        = "/tmp/chip_kvs_bridge"
PAIRING_CODE    = "34970112332"

# GPIO state tracking
gpio_state = {17: False, 27: False, 22: False}

def print_gpio_status():
    print("\n┌─────────────────────────────────┐")
    print("│        GPIO STATUS              │")
    print("├──────────────┬──────────────────┤")
    print(f"│ First Plug   │ GPIO 17 → {'⚡ ON ' if gpio_state[17] else '🔌 OFF'}          │")
    print(f"│ Second Plug  │ GPIO 27 → {'⚡ ON ' if gpio_state[27] else '🔌 OFF'}          │")
    print(f"│ Third Plug   │ GPIO 22 → {'⚡ ON ' if gpio_state[22] else '🔌 OFF'}          │")
    print("└──────────────┴──────────────────┘\n")

def parse_gpio_line(line):
    """Parse log lines to track GPIO state changes triggered by gpioset in C++."""
    # These log lines appear when the bridge app's set_gpio() is called
    changed = False
    if "First Plug" in line and "OnOff" in line:
        if "true" in line.lower() or "value: 1" in line.lower():
            gpio_state[17] = True; changed = True
        elif "false" in line.lower() or "value: 0" in line.lower():
            gpio_state[17] = False; changed = True
    elif "Second Plug" in line and "OnOff" in line:
        if "true" in line.lower() or "value: 1" in line.lower():
            gpio_state[27] = True; changed = True
        elif "false" in line.lower() or "value: 0" in line.lower():
            gpio_state[27] = False; changed = True
    elif "Third Plug" in line and "OnOff" in line:
        if "true" in line.lower() or "value: 1" in line.lower():
            gpio_state[22] = True; changed = True
        elif "false" in line.lower() or "value: 0" in line.lower():
            gpio_state[22] = False; changed = True
    return changed

proc = None

def shutdown(signum=None, frame=None):
    print("\n[SHUTDOWN] Stopping bridge app...")
    if proc:
        proc.terminate()
    sys.exit(0)

signal.signal(signal.SIGINT,  shutdown)
signal.signal(signal.SIGTERM, shutdown)

# Check binary exists
if not os.path.exists(CHIP_BRIDGE_APP):
    print(f"[ERROR] Binary not found: {CHIP_BRIDGE_APP}")
    print("Build it first with: ./scripts/build/build_examples.py --target linux-arm64-bridge --ninja-jobs 1 build")
    sys.exit(1)

print("=" * 50)
print("  Matter Bridge GPIO Controller")
print("=" * 50)
print(f"  First Plug  → GPIO 17")
print(f"  Second Plug → GPIO 27")
print(f"  Third Plug  → GPIO 22")
print(f"  Pairing Code: {PAIRING_CODE}")
print("=" * 50)
print("Pair in Alexa: Add Device → Other → Matter\n")

proc = subprocess.Popen(
    [
        CHIP_BRIDGE_APP,
        "--discriminator", DISCRIMINATOR,
        "--passcode",      PASSCODE,
        "--KVS",           KVS_PATH,
    ],
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    text=True,
    bufsize=1,
)

print(f"[STARTED] chip-bridge-app PID: {proc.pid}\n")

try:
    for line in proc.stdout:
        sys.stdout.write(line)
        sys.stdout.flush()
        if parse_gpio_line(line):
            print_gpio_status()
except Exception as e:
    print(f"[ERROR] {e}")
finally:
    shutdown()
