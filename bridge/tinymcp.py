#!/usr/bin/env python3
#
# tinymcp v0.2
# The Model Context Protocol server as a library any firmware can carry.
# http://tinymcp.org
#
# Authors: Ambuj Varshney <ambuj@tiku-os.org>
#
# tinymcp.py - the bridge between an MCP client's stdio and a device: stdin
# lines become channel 0xF3 frames on a serial port (or lines on a TCP
# connection), frames come back as stdout lines, and the text around them,
# a banner or a person's shell session, goes to stderr.
#
# SPDX-License-Identifier: Apache-2.0

import argparse
import json
import os
import socket
import subprocess
import sys
import threading

END = 0xC0
ESC = 0xDB
ESC_END = 0xDC
ESC_ESC = 0xDD
ESC_NUL = 0xDE
MARK_MCP = 0xF3

OPEN_TIMEOUT = 8.0   # macOS blocks about 30 s on a CDC port whose firmware is wedged


def escape(payload):
    """The SLIP escapes of payload: END, ESC and NUL become two bytes."""
    out = bytearray()
    for b in payload:
        if b == END:
            out += bytes((ESC, ESC_END))
        elif b == ESC:
            out += bytes((ESC, ESC_ESC))
        elif b == 0:
            out += bytes((ESC, ESC_NUL))
        else:
            out.append(b)
    return bytes(out)


def frame(payload, marker=MARK_MCP):
    """One frame on the wire: END, the marker bare, the payload escaped, END."""
    return bytes((END, marker)) + escape(payload) + bytes((END,))


class Splitter:
    """Cut frames out of a byte stream the way the device's decoder does.

    feed() returns (text, frames): the bytes outside any frame, and the
    payloads of the frames with the marker, delivered whole.  A byte after
    an END that is not the marker is text; frames with another marker are
    dropped.
    """

    def __init__(self, marker=MARK_MCP):
        self.marker = marker
        self._armed = False
        self._in_frame = False
        self._ours = False
        self._esc = False
        self._buf = bytearray()

    def feed(self, data):
        text = bytearray()
        frames = []
        for b in data:
            if b == END:
                if self._in_frame and self._ours and self._buf:
                    frames.append(bytes(self._buf))
                self._in_frame = False
                self._armed = True
                self._buf = bytearray()
                self._esc = False
                continue
            if self._armed:
                self._armed = False
                if b == self.marker or 0xF0 <= b <= 0xFF or 0x40 <= b <= 0x4F:
                    self._in_frame = True
                    self._ours = b == self.marker
                    self._buf = bytearray()
                    self._esc = False
                    continue
            if not self._in_frame:
                text.append(b)
                continue
            if self._esc:
                self._esc = False
                b = {ESC_END: END, ESC_ESC: ESC, ESC_NUL: 0}.get(b, b)
            elif b == ESC:
                self._esc = True
                continue
            self._buf.append(b)
        return bytes(text), frames


def holders(port):
    """The processes that hold port open, by pid, or an empty list."""
    try:
        out = subprocess.run(["lsof", "-t", port], capture_output=True,
                             text=True, timeout=5).stdout.split()
    except (OSError, subprocess.SubprocessError):
        return []
    return [p for p in out if p != str(os.getpid())]


def open_serial(port, baud):
    """A pyserial port opened in a thread, so a wedged device cannot hang
    the bridge: after OPEN_TIMEOUT the bridge says which port and exits."""
    import serial  # pyserial; imported here so --tcp needs no serial

    held = holders(port)
    if held:
        raise SystemExit(f"tinymcp: {port} is held by pid {' '.join(held)}")
    result = {}

    def work():
        try:
            # DTR and RTS asserted on open, pyserial's default: an ESP
            # board's USB bridge resets the chip when one line drops while
            # the other stays up, and an open with both low did so too.
            s = serial.Serial(port, baud, timeout=0.05)
            result["port"] = s
        except Exception as e:  # noqa: BLE001 - reported below
            result["error"] = e

    t = threading.Thread(target=work, daemon=True)
    t.start()
    t.join(OPEN_TIMEOUT)
    if t.is_alive():
        raise SystemExit(f"tinymcp: {port} did not open within {OPEN_TIMEOUT:.0f} s "
                         "(a CDC port whose firmware is not answering)")
    if "error" in result:
        raise SystemExit(f"tinymcp: {port}: {result['error']}")
    return result["port"]


class SerialLink:
    """Frames on a serial port."""

    def __init__(self, port, baud, raw):
        self.ser = open_serial(port, baud)
        self.raw = raw
        self.split = Splitter()

    def send(self, line):
        self.ser.write(line + b"\n" if self.raw else frame(line))
        self.ser.flush()

    def recv(self):
        """(text, messages) read in one timeout slice; (None, None) once closed."""
        try:
            data = self.ser.read(4096)
        except Exception:  # noqa: BLE001 - the port went away
            return None, None
        if not data:
            return b"", []
        if self.raw:
            return b"", [data]
        return self.split.feed(data)

    def close(self):
        self.ser.close()


class TcpLink:
    """Newline-delimited messages on a TCP connection."""

    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=10)
        self.sock.settimeout(0.05)

    def send(self, line):
        self.sock.sendall(line + b"\n")

    def recv(self):
        try:
            data = self.sock.recv(4096)
        except socket.timeout:
            return b"", []
        except OSError:
            return None, None
        if not data:
            return None, None
        return b"", [data]

    def close(self):
        self.sock.close()


def listen_ids(line):
    """The id of a subscriptions/listen request in line, or None."""
    try:
        msg = json.loads(line)
    except ValueError:
        return None
    if isinstance(msg, dict) and msg.get("method") == "subscriptions/listen" \
            and "id" in msg:
        return msg["id"]
    return None


def cancelled(request_id):
    return json.dumps({"jsonrpc": "2.0", "method": "notifications/cancelled",
                       "params": {"requestId": request_id}}).encode()


def main():
    ap = argparse.ArgumentParser(description="MCP stdio to a tinymcp device")
    ap.add_argument("port", nargs="?", help="serial port, e.g. /dev/cu.usbmodem1101")
    ap.add_argument("--tcp", metavar="HOST:PORT", help="a device's mcp listen port instead")
    ap.add_argument("--raw", action="store_true",
                    help="newline-delimited bytes on the serial port, no frames")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--trace", action="store_true", help="every message to stderr")
    args = ap.parse_args()
    if bool(args.port) == bool(args.tcp):
        ap.error("give a serial port or --tcp HOST:PORT")

    if args.tcp:
        host, _, port = args.tcp.rpartition(":")
        link = TcpLink(host or "127.0.0.1", int(port))
        where = args.tcp
    else:
        link = SerialLink(args.port, args.baud, args.raw)
        where = args.port
    sys.stderr.write(f"tinymcp: opened {where}\n")
    sys.stderr.flush()

    out = sys.stdout.buffer
    err = sys.stderr.buffer
    listens = []
    done = threading.Event()
    pending = bytearray()   # a raw/TCP message split across reads

    def reader():
        while not done.is_set():
            text, msgs = link.recv()
            if text is None:
                err.write(b"tinymcp: the device closed the connection\n")
                err.flush()
                done.set()
                return
            if text:
                err.write(text)
                err.flush()
            for m in msgs:
                if args.raw or args.tcp:
                    pending.extend(m)
                    while b"\n" in pending:
                        line, _, rest = bytes(pending).partition(b"\n")
                        pending[:] = rest
                        emit(line.rstrip(b"\r"))
                else:
                    emit(m)

    def emit(msg):
        if not msg.strip():
            return
        if args.trace:
            err.write(b"<- " + msg + b"\n")
            err.flush()
        out.write(msg + b"\n")
        out.flush()

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    try:
        for line in sys.stdin.buffer:
            line = line.rstrip(b"\r\n")
            if not line.strip():
                continue
            if args.trace:
                err.write(b"-> " + line + b"\n")
                err.flush()
            lid = listen_ids(line)
            if lid is not None:
                listens.append(lid)
            link.send(line)
            if done.is_set():
                break
    except KeyboardInterrupt:
        pass
    finally:
        # The device has no connection to see close: its listens are told.
        for lid in listens:
            try:
                link.send(cancelled(lid))
            except Exception:  # noqa: BLE001 - leaving anyway
                break
        done.set()
        t.join(0.5)
        link.close()


if __name__ == "__main__":
    main()
