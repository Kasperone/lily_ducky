#!/usr/bin/env python3
"""probe_reset.py — capture boot output around an RTS reset pulse.

Usage: probe_reset.py [port] [logfile]

Opens the port, drains any pending output, pulses RTS (reset), then reads
for ~12 s and appends everything to the log file with a marker. Reports
whether the port stayed alive across the reset (USB re-enumeration check).
"""
import os
import sys
import time
import struct
import select
import fcntl
import termios

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
LOG = sys.argv[2] if len(sys.argv) > 2 else "/tmp/lily_probe.log"

fd = os.open(PORT, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
attrs = termios.tcgetattr(fd)
attrs[0] = 0
attrs[1] = 0
attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
attrs[3] = 0
attrs[4] = termios.B115200
attrs[5] = termios.B115200
attrs[6][termios.VMIN] = 0
attrs[6][termios.VTIME] = 0
termios.tcsetattr(fd, termios.TCSANOW, attrs)

TIOCM_RTS = termios.TIOCM_RTS
TIOCM_DTR = termios.TIOCM_DTR

def get_modem():
    return struct.unpack("I", fcntl.ioctl(fd, termios.TIOCMGET, b"\0" * 4))[0]

def set_modem(rts, dtr):
    v = get_modem()
    v = (v | TIOCM_RTS) if rts else (v & ~TIOCM_RTS)
    v = (v | TIOCM_DTR) if dtr else (v & ~TIOCM_DTR)
    fcntl.ioctl(fd, termios.TIOCMSET, struct.pack("I", v))

def drain(deadline_s, out):
    end = time.monotonic() + deadline_s
    while time.monotonic() < end:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            try:
                data = os.read(fd, 4096)
            except OSError as e:
                out.write(b"\n[probe] READ ERROR: " + str(e).encode() + b"\n")
                return False
            if data:
                out.write(data)
    return True

with open(LOG, "ab", buffering=0) as log:
    log.write(b"\n=== probe start " +
              time.strftime("%H:%M:%S").encode() + b" ===\n")
    # pre-drain any stale output
    drain(1.0, log)
    try:
        log.write(b"[probe] modem lines: " +
                  hex(get_modem()).encode() + b"\n")
    except OSError as e:
        log.write(b"[probe] TIOCMGET failed: " + str(e).encode() + b"\n")
    # pulse reset (DTR cleared so this is an app reset, not download-mode)
    set_modem(False, False)
    time.sleep(0.05)
    set_modem(True, False)
    time.sleep(0.1)
    set_modem(False, False)
    log.write(b"[probe] reset pulsed " +
              time.strftime("%H:%M:%S").encode() + b"\n")
    ok = drain(12.0, log)
    log.write(b"[probe] capture done, port_ok=" +
              str(ok).encode() + b"\n")
print("probe complete; log:", LOG)
