#!/usr/bin/env python3
#
# tinymcp v0.7
# The Model Context Protocol server as a library any firmware can carry.
# http://tinymcp.org
#
# Authors: Ambuj Varshney <ambuj@tiku-os.org>
#
# slip_tcp.py - a TCP client of a tikuOS board over SLIP frames on its own
# console line, with no TUN device and no root, and a relay from a local
# port to it: the network a board test needs, standing in for a real one.
#
# SPDX-License-Identifier: Apache-2.0

"""TCP to a board through its console line.

The board carries IP packets on the console as frames between 0xC0 bytes
once its `slip` command is on.  SlipTcp opens one connection to the board,
acknowledges each segment it receives, and keeps within the window the
board advertises.  The cable loses nothing, so nothing is retransmitted:
this is a test fixture, not a stack.  relay() accepts local TCP clients
one at a time and joins each to a board connection.
"""

import random
import select
import socket
import struct
import threading
import time

END, ESC, ESC_END, ESC_ESC, ESC_NUL = 0xC0, 0xDB, 0xDC, 0xDD, 0xDE
FIN, SYN, RST, PSH, ACK = 0x01, 0x02, 0x04, 0x08, 0x10
HOST_IP, BOARD_IP = "172.16.7.1", "172.16.7.2"


def checksum(data):
    if len(data) % 2:
        data += b"\0"
    total = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def frame(pkt):
    """One packet as a console frame: END, the bytes escaped, END."""
    out = bytearray([END])
    for b in pkt:
        if b == END:
            out += bytes((ESC, ESC_END))
        elif b == ESC:
            out += bytes((ESC, ESC_ESC))
        elif b == 0:
            out += bytes((ESC, ESC_NUL))
        else:
            out.append(b)
    out.append(END)
    return bytes(out)


class Wire:
    """The console line: IP packets out of its frames, the rest dropped."""

    def __init__(self, ser):
        self.ser = ser
        self.lock = threading.Lock()
        self.buf = bytearray()
        self.inside = False
        self.esc = False
        self.packets = []

    def send(self, pkt):
        with self.lock:
            self.ser.write(frame(pkt))
            self.ser.flush()

    def type(self, line):
        """A line typed at the board's shell."""
        with self.lock:
            self.ser.write(line.encode() + b"\r")
            self.ser.flush()

    def poll(self, secs=0.02):
        """Read what waits; the IP packets that completed."""
        end = time.monotonic() + secs
        while True:
            n = self.ser.in_waiting
            data = self.ser.read(n) if n else b""
            for b in data:
                self.feed(b)
            if self.packets or time.monotonic() >= end:
                break
            time.sleep(0.002)
        out, self.packets = self.packets, []
        return out

    def feed(self, b):
        if b == END:
            if self.inside and len(self.buf) >= 20 and self.buf[0] >> 4 == 4:
                self.packets.append(bytes(self.buf))
            self.inside = True
            self.buf = bytearray()
            self.esc = False
            return
        if not self.inside:
            return
        if self.esc:
            self.esc = False
            b = {ESC_END: END, ESC_ESC: ESC, ESC_NUL: 0}.get(b, b)
        elif b == ESC:
            self.esc = True
            return
        self.buf.append(b)


class SlipTcp:
    """One TCP connection to the board, opened by us."""

    def __init__(self, wire, port, sport=None, mss=536, window=4096):
        self.w = wire
        self.dport = port
        self.sport = sport or random.randint(40000, 60000)
        self.mss = mss
        self.window = window
        self.snd_nxt = random.randint(0, 0xFFFFFFF)
        self.snd_una = self.snd_nxt
        self.snd_wnd = 0
        self.peer_mss = 536
        self.rcv_nxt = 0
        self.rx = bytearray()
        self.state = "closed"
        self.ipid = 0

    def _send(self, flags, payload=b"", opts=b""):
        doff = (20 + len(opts)) // 4
        tcp = struct.pack("!HHIIBBHHH", self.sport, self.dport, self.snd_nxt,
                          self.rcv_nxt, doff << 4, flags, self.window, 0, 0)
        tcp += opts + payload
        src, dst = socket.inet_aton(HOST_IP), socket.inet_aton(BOARD_IP)
        pseudo = src + dst + struct.pack("!BBH", 0, 6, len(tcp))
        tcp = tcp[:16] + struct.pack("!H", checksum(pseudo + tcp)) + tcp[18:]
        self.ipid = (self.ipid + 1) & 0xFFFF
        ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(tcp), self.ipid,
                         0x4000, 64, 6, 0, src, dst)
        ip = ip[:10] + struct.pack("!H", checksum(ip)) + ip[12:]
        self.w.send(ip + tcp)
        if flags & (SYN | FIN):
            self.snd_nxt = (self.snd_nxt + 1) & 0xFFFFFFFF
        self.snd_nxt = (self.snd_nxt + len(payload)) & 0xFFFFFFFF

    def _segment(self, pkt):
        ihl = (pkt[0] & 15) * 4
        if pkt[9] != 6:
            return
        tcp = pkt[ihl:struct.unpack("!H", pkt[2:4])[0]]
        sport, dport, seq, ack, doff, flags, wnd = struct.unpack("!HHIIBBH", tcp[:16])
        if sport != self.dport or dport != self.sport:
            return
        payload = tcp[(doff >> 4) * 4:]
        if flags & RST:
            self.state = "reset"
            return
        if self.state == "syn-sent":
            if flags & SYN and flags & ACK:
                self.rcv_nxt = (seq + 1) & 0xFFFFFFFF
                self.snd_una, self.snd_wnd = ack, wnd
                opts = tcp[20:(doff >> 4) * 4]
                if len(opts) >= 4 and opts[0] == 2:
                    self.peer_mss = struct.unpack("!H", opts[2:4])[0]
                self.state = "open"
                self._send(ACK)
            return
        if flags & ACK:
            self.snd_una, self.snd_wnd = ack, wnd
        if payload:
            if seq == self.rcv_nxt:
                self.rx += payload
                self.rcv_nxt = (self.rcv_nxt + len(payload)) & 0xFFFFFFFF
            self._send(ACK)         # a repeat is acknowledged again
        if flags & FIN and (seq + len(payload)) & 0xFFFFFFFF == self.rcv_nxt:
            self.rcv_nxt = (self.rcv_nxt + 1) & 0xFFFFFFFF
            self._send(ACK)
            self.state = "peer-closed"

    def turn(self, secs=0.02):
        for pkt in self.w.poll(secs):
            self._segment(pkt)

    def connect(self, timeout=5.0):
        self.state = "syn-sent"
        self._send(SYN, opts=struct.pack("!BBH", 2, 4, self.mss))
        end = time.monotonic() + timeout
        while self.state == "syn-sent" and time.monotonic() < end:
            self.turn(0.05)
        return self.state == "open"

    def send(self, data, timeout=10.0):
        """data in segments within the board's window; False if it stalled."""
        end = time.monotonic() + timeout
        while data:
            in_flight = (self.snd_nxt - self.snd_una) & 0xFFFFFFFF
            room = min(self.peer_mss, self.snd_wnd - in_flight)
            if self.state != "open" or time.monotonic() > end:
                return False
            if room <= 0:
                self.turn(0.02)
                continue
            self._send(PSH | ACK, data[:room])
            data = data[room:]
            self.turn(0)
        return True

    def take(self):
        out, self.rx = bytes(self.rx), bytearray()
        return out

    def close(self):
        """FIN, then until the board's FIN is acknowledged too, so it keeps no
        half-closed connection waiting for an ACK."""
        if self.state in ("open", "peer-closed"):
            self._send(FIN | ACK)
            end = time.monotonic() + 3
            while (self.snd_una != self.snd_nxt or self.state != "peer-closed") \
                    and self.state != "reset" and time.monotonic() < end:
                self.turn(0.05)
        self.state = "closed"


def relay(wire, board_port, stop, ready):
    """Accept local clients one at a time on a free port, each joined to a
    connection to board_port; ready gets the port; until stop is set."""
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    srv.settimeout(0.2)
    ready.append(srv.getsockname()[1])
    while not stop.is_set():
        try:
            client, _ = srv.accept()
        except socket.timeout:
            continue
        tcp = SlipTcp(wire, board_port)
        if not tcp.connect():
            client.close()
            continue
        client.setblocking(False)
        while not stop.is_set() and tcp.state == "open":
            r, _, _ = select.select([client], [], [], 0)
            if r:
                try:
                    data = client.recv(4096)
                except OSError:
                    data = b""
                if not data:
                    break
                tcp.send(data)
            tcp.turn(0.01)
            got = tcp.take()
            if got:
                client.setblocking(True)
                client.sendall(got)
                client.setblocking(False)
        tcp.close()
        client.close()
    srv.close()
