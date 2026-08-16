#!/usr/bin/env python3
"""Minimal rmt protocol server on stdio for mutar tests (O/R/W/L/C)."""
from __future__ import annotations

import os
import sys


def reply_ok(n: int = 0) -> None:
    sys.stdout.write(f"A{n}\n")
    sys.stdout.flush()


def reply_err(errno: int = 22, msg: str = "error") -> None:
    sys.stdout.write(f"E{errno}\n{msg}\n")
    sys.stdout.flush()


def read_line() -> str | None:
    line = sys.stdin.buffer.readline()
    if not line:
        return None
    return line.decode("utf-8", errors="surrogateescape").rstrip("\n")


def main() -> int:
    fd = -1
    while True:
        line = read_line()
        if line is None:
            break
        if not line:
            continue
        cmd, arg = line[0], line[1:]
        if cmd == "O":
            flags_line = read_line()
            if flags_line is None:
                reply_err(5, "I/O error")
                break
            flags_s = flags_line.split()[0]
            try:
                flags = int(flags_s)
            except ValueError:
                reply_err(22, "invalid open mode")
                continue
            if fd >= 0:
                os.close(fd)
                fd = -1
            path = arg
            acc = flags & 0x3
            creat = bool(flags & 0x40)
            trunc = bool(flags & 0x200)
            oflags = 0
            if acc == 0:
                oflags = os.O_RDONLY
            elif acc == 1:
                oflags = os.O_WRONLY
            else:
                oflags = os.O_RDWR
            if creat:
                oflags |= os.O_CREAT
            if trunc:
                oflags |= os.O_TRUNC
            try:
                fd = os.open(path, oflags, 0o666)
                reply_ok(0)
            except OSError as e:
                reply_err(e.errno or 2, e.strerror or "open failed")
        elif cmd == "C":
            if fd >= 0:
                os.close(fd)
                fd = -1
            reply_ok(0)
        elif cmd == "L":
            off_line = read_line()
            if off_line is None:
                reply_err(5, "I/O error")
                break
            if fd < 0:
                reply_err(9, "Bad file descriptor")
                continue
            try:
                whence = int(arg)
                offset = int(off_line)
                wmap = {0: os.SEEK_SET, 1: os.SEEK_CUR, 2: os.SEEK_END}
                pos = os.lseek(fd, offset, wmap.get(whence, os.SEEK_SET))
                reply_ok(pos)
            except OSError as e:
                reply_err(e.errno or 22, e.strerror or "seek failed")
            except ValueError:
                reply_err(22, "Invalid seek")
        elif cmd == "R":
            if fd < 0:
                reply_err(9, "Bad file descriptor")
                continue
            try:
                n = int(arg)
                data = os.read(fd, n)
                sys.stdout.write(f"A{len(data)}\n")
                sys.stdout.flush()
                if data:
                    sys.stdout.buffer.write(data)
                    sys.stdout.buffer.flush()
            except OSError as e:
                reply_err(e.errno or 5, e.strerror or "read failed")
            except ValueError:
                reply_err(22, "invalid count")
        elif cmd == "W":
            if fd < 0:
                reply_err(9, "Bad file descriptor")
                continue
            try:
                n = int(arg)
                buf = b""
                while len(buf) < n:
                    chunk = sys.stdin.buffer.read(n - len(buf))
                    if not chunk:
                        break
                    buf += chunk
                written = os.write(fd, buf) if buf else 0
                reply_ok(written)
            except OSError as e:
                reply_err(e.errno or 5, e.strerror or "write failed")
            except ValueError:
                reply_err(22, "invalid count")
        elif cmd == "S":
            reply_err(25, "Inappropriate ioctl for device")
        else:
            reply_err(22, "Garbage command")
    if fd >= 0:
        os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
