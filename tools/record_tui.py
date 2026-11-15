"""Record a terminal program's raw output in a pseudo-terminal, for tools/vtcheck.

usage: python3 record_tui.py out.bin <cols> <rows> <seconds> <command...>

Runs the command at the given size, records everything it prints for the given
number of seconds, then presses Ctrl+C twice and stops.
"""
import fcntl
import os
import pty
import select
import struct
import sys
import termios
import time

out, cols, rows, seconds, *cmd = sys.argv[1:]
cols, rows, seconds = int(cols), int(rows), float(seconds)

pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm-256color"
    os.execvp(cmd[0], cmd)

fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
data = bytearray()
end = time.time() + seconds
sent = 0
while True:
    now = time.time()
    if now > end and sent < 2:
        os.write(fd, b"\x03")
        sent += 1
        end = now + 1.0
    elif now > end:
        break
    ready, _, _ = select.select([fd], [], [], 0.1)
    if ready:
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break
        if not chunk:
            break
        data += chunk

open(out, "wb").write(bytes(data))
try:
    os.kill(pid, 9)
except ProcessLookupError:
    pass
print(f"recorded {len(data)} bytes")
