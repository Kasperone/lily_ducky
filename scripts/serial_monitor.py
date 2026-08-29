#!/usr/bin/env python3
"""serial_monitor.py — dumb serial log capture for LilyDucky bring-up.

Usage: serial_monitor.py [port] [logfile]

Opens the port raw (termios, stdlib only — no pyserial needed; USB CDC
ignores baud rate anyway) and appends every byte received to the log file.
Runs until killed. A timestamped marker line is written at start.
"""
import os
import sys
import time
import termios

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
LOG = sys.argv[2] if len(sys.argv) > 2 else "/tmp/lily_boot.log"

fd = os.open(PORT, os.O_RDWR | os.O_NOCTTY)
attrs = termios.tcgetattr(fd)
attrs[0] = 0                                        # iflag: raw
attrs[1] = 0                                        # oflag: raw
attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
attrs[3] = 0                                        # lflag: raw
attrs[4] = termios.B115200
attrs[5] = termios.B115200
attrs[6][termios.VMIN] = 1
attrs[6][termios.VTIME] = 0
termios.tcsetattr(fd, termios.TCSANOW, attrs)
termios.tcflush(fd, termios.TCIOFLUSH)

with open(LOG, "ab", buffering=0) as log:
    log.write(b"\n=== monitor start " +
              time.strftime("%Y-%m-%d %H:%M:%S").encode() + b" ===\n")
    while True:
        data = os.read(fd, 4096)   # blocks until at least 1 byte
        if data:
            log.write(data)
