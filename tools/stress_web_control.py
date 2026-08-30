#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the dongle's binary WebSocket controller without dependencies."""

import argparse
import base64
import hashlib
import http.client
import json
import os
import socket
import struct
import time


class Controller:
    def __init__(self, host: str):
        self.socket = socket.create_connection((host, 80), timeout=2)
        key = base64.b64encode(os.urandom(16)).decode()
        request = (
            f"GET /ws HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\n"
            f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self.socket.sendall(request.encode())
        response = b""
        while b"\r\n\r\n" not in response:
            response += self.socket.recv(1024)
        if not response.startswith(b"HTTP/1.1 101"):
            raise RuntimeError(response.decode(errors="replace"))
        expected = base64.b64encode(hashlib.sha1(
            (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()
        ).digest())
        if expected.lower() not in response.lower():
            raise RuntimeError("bad Sec-WebSocket-Accept")

    def send(self, mask: int) -> None:
        payload = struct.pack("<I", mask)
        key = os.urandom(4)
        encoded = bytes(value ^ key[i & 3] for i, value in enumerate(payload))
        self.socket.sendall(b"\x82\x84" + key + encoded)

    def expect_ack(self) -> None:
        """Consume frames until the one-byte input acknowledgement arrives.

        A fresh controller also receives the current music state, so treating
        the first binary frame as the ACK would make the test race the engine.
        """
        while True:
            header = self.socket.recv(2)
            if len(header) != 2 or header[0] != 0x82:
                raise RuntimeError(f"bad WebSocket binary header: {header!r}")
            length = header[1] & 0x7f
            if length == 126:
                length = struct.unpack(">H", self.socket.recv(2))[0]
            elif length == 127:
                length = struct.unpack(">Q", self.socket.recv(8))[0]
            payload = b""
            while len(payload) < length:
                payload += self.socket.recv(length - len(payload))
            if payload == b"\xac":
                return
            if not payload.startswith(b"\xda"):
                raise RuntimeError(f"unexpected WebSocket payload: {payload!r}")

    def abort(self) -> None:
        try:
            self.socket.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.socket.close()


def status(host: str) -> dict:
    connection = http.client.HTTPConnection(host, timeout=2)
    connection.request("GET", "/api/status")
    response = connection.getresponse()
    result = json.loads(response.read())
    connection.close()
    return result


def wait_released(host: str, timeout: float) -> tuple[dict, float]:
    started = time.monotonic()
    while time.monotonic() - started < timeout:
        current = status(host)
        if not current["remote"]:
            return current, time.monotonic() - started
        time.sleep(0.025)
    raise RuntimeError(f"remote input remained active for {timeout:.1f}s")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("host", nargs="?", default="192.168.4.1")
    parser.add_argument("--packets", type=int, default=2000)
    parser.add_argument("--disconnects", type=int, default=40)
    parser.add_argument(
        "--hold-fire-seconds", type=float, default=0,
        help="hold the attack bit and consume every server ACK for this long",
    )
    args = parser.parse_args()

    before = status(args.host)
    if args.hold_fire_seconds > 0:
        controller = Controller(args.host)
        deadline = time.monotonic() + args.hold_fire_seconds
        acknowledgements = 0
        while time.monotonic() < deadline:
            controller.send(1 << 4)
            controller.expect_ack()
            acknowledgements += 1
            time.sleep(0.1)
        controller.send(0)
        controller.expect_ack()
        controller.abort()
        after = status(args.host)
        print(json.dumps({
            "held_fire_seconds": args.hold_fire_seconds,
            "acknowledgements": acknowledgements + 1,
            "kills_before": before.get("kills"),
            "kills_after": after.get("kills"),
            "weapon_after": after.get("weapon"),
            "heap_before": before["heap"],
            "heap_after": after["heap"],
            "largest_after": after["largest"],
            "fps": after["fps"],
        }, indent=2))
        return

    controller = Controller(args.host)
    for packet in range(args.packets):
        controller.send(1 << (packet & 3))
        controller.send(0)
    controller.abort()

    # Measure the normal failure case without a connection storm obscuring the
    # firmware's lease/disconnect latency.
    wait_released(args.host, 2.0)
    controller = Controller(args.host)
    controller.send(1 << 2)
    controller.abort()
    _, single_release_seconds = wait_released(args.host, 1.0)

    for cycle in range(args.disconnects):
        controller = Controller(args.host)
        controller.send(1 << (cycle & 3))
        controller.abort()  # Deliberately disappear while a direction is held.
        time.sleep(0.015)

    # A deliberately abusive reconnect storm can leave already-closed sockets
    # queued behind the single controller endpoint. Let the server drain them;
    # every processed source still has the independent 350 ms safety lease.
    after, storm_release_seconds = wait_released(args.host, 5.0)
    if after["fps"] != 35:
        raise RuntimeError(f"unexpected engine rate: {after['fps']}")
    print(json.dumps({
        "packets": args.packets * 2,
        "held_disconnects": args.disconnects,
        "single_release_ms": round(single_release_seconds * 1000, 1),
        "storm_drain_ms": round(storm_release_seconds * 1000, 1),
        "heap_before": before["heap"],
        "heap_after": after["heap"],
        "largest_after": after["largest"],
        "remote_after": after["remote"],
        "fps": after["fps"],
    }, indent=2))


if __name__ == "__main__":
    main()
