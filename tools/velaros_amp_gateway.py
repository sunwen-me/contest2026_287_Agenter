#!/usr/bin/env python3
"""No-board Linux-side AMP gateway protocol self-test.

The socketpair is only a deterministic transport substitute. A K1 deployment
must replace it with RPMsg/OpenAMP and retain the same fixed little-endian
frame contract.
"""

from __future__ import annotations

import argparse
import socket
import struct
import zlib
from dataclasses import dataclass

MAGIC = 0x56414D50
VERSION = 1
MAX_PAYLOAD = 64
FRAME_FORMAT = "<IHHIQHH64sI"
FRAME_SIZE = struct.calcsize(FRAME_FORMAT)
CRC_OFFSET = FRAME_SIZE - 4

HEARTBEAT = 1
MOTION_COMMAND = 2
STATUS = 3
EMERGENCY_STOP = 4
FLAG_STOP = 2

STATE_DISCONNECTED = 0
STATE_READY = 1
STATE_ACTIVE = 2
STATE_EMERGENCY_STOP = 3
STATE_FAULT = 4

FAULT_NONE = 0
FAULT_BAD_FRAME = 1
FAULT_BAD_SEQUENCE = 2
FAULT_COMMAND_TIMEOUT = 3
FAULT_PEER_TIMEOUT = 4
FAULT_REMOTE_EMERGENCY_STOP = 5


@dataclass(frozen=True)
class Frame:
    message_type: int
    flags: int
    sequence: int
    timestamp_us: int
    payload: bytes

    def pack(self) -> bytes:
        if not 0 < self.sequence <= 0xFFFFFFFF:
            raise ValueError("sequence must be non-zero uint32")
        if len(self.payload) > MAX_PAYLOAD:
            raise ValueError("AMP payload exceeds fixed capacity")
        body = struct.pack(
            FRAME_FORMAT,
            MAGIC,
            VERSION,
            self.message_type,
            self.sequence,
            self.timestamp_us,
            len(self.payload),
            self.flags,
            self.payload.ljust(MAX_PAYLOAD, b"\0"),
            0,
        )
        crc = zlib.crc32(body[:CRC_OFFSET]) & 0xFFFFFFFF
        return body[:CRC_OFFSET] + struct.pack("<I", crc)

    @staticmethod
    def unpack(data: bytes) -> "Frame":
        if len(data) != FRAME_SIZE:
            raise ValueError("AMP frame size mismatch")
        values = struct.unpack(FRAME_FORMAT, data)
        magic, version, message_type, sequence, timestamp_us, payload_length, flags, payload, crc = values
        if magic != MAGIC or version != VERSION or sequence == 0:
            raise ValueError("AMP frame header mismatch")
        if not 0 <= payload_length <= MAX_PAYLOAD:
            raise ValueError("AMP payload length mismatch")
        expected_crc = zlib.crc32(data[:CRC_OFFSET]) & 0xFFFFFFFF
        if crc != expected_crc:
            raise ValueError("AMP CRC mismatch")
        return Frame(message_type, flags, sequence, timestamp_us, payload[:payload_length])


def make_heartbeat(sequence: int, timestamp_us: int) -> Frame:
    return Frame(HEARTBEAT, 0, sequence, timestamp_us, struct.pack("<II", 250, 1))


def make_motion(sequence: int, timestamp_us: int) -> Frame:
    payload = struct.pack("<ffII", 0.35, -0.20, 250, 0)
    return Frame(MOTION_COMMAND, 0, sequence, timestamp_us, payload)


class AmpGateway:
    def __init__(self) -> None:
        self.state = STATE_DISCONNECTED
        self.fault = FAULT_NONE
        self.last_rx_sequence = 0
        self.last_command_sequence = 0
        self.last_rx_timestamp_us = 0
        self.last_command_timestamp_us = 0
        self.command_timeout_ms = 250
        self.heartbeat_timeout_ms = 1000
        self.peer_alive = False
        self.emergency_stop = False

    def accept(self, frame: Frame, now_us: int) -> None:
        if frame.sequence <= self.last_rx_sequence:
            self.state = STATE_FAULT
            self.fault = FAULT_BAD_SEQUENCE
            self.stop(FAULT_BAD_SEQUENCE)
            raise ValueError("AMP sequence is not monotonic")
        self.last_rx_sequence = frame.sequence
        self.last_rx_timestamp_us = now_us
        self.peer_alive = True

        if frame.message_type == HEARTBEAT:
            period_ms, _ = struct.unpack("<II", frame.payload)
            self.heartbeat_timeout_ms = max(1000, period_ms * 3)
            if not self.emergency_stop and self.state != STATE_ACTIVE:
                self.state = STATE_READY
                self.fault = FAULT_NONE
        elif frame.message_type == MOTION_COMMAND:
            linear_x, angular_z, timeout_ms, command_flags = struct.unpack(
                "<ffII", frame.payload
            )
            self.last_command_sequence = frame.sequence
            self.last_command_timestamp_us = now_us
            self.command_timeout_ms = timeout_ms or 250
            if frame.flags & FLAG_STOP or command_flags & FLAG_STOP:
                self.stop(FAULT_NONE)
            elif not self.emergency_stop:
                self.state = STATE_ACTIVE
                self.fault = FAULT_NONE
                self.command = (linear_x, angular_z)
        elif frame.message_type == EMERGENCY_STOP:
            self.stop(FAULT_REMOTE_EMERGENCY_STOP)
        else:
            raise ValueError("gateway only accepts heartbeat, motion, or stop")

    def stop(self, fault: int) -> None:
        self.state = STATE_EMERGENCY_STOP
        self.fault = fault
        self.emergency_stop = True
        self.command = (0.0, 0.0)

    def tick(self, now_us: int) -> bool:
        if (
            self.state == STATE_ACTIVE
            and now_us > self.last_command_timestamp_us
            and now_us - self.last_command_timestamp_us > self.command_timeout_ms * 1000
        ):
            self.stop(FAULT_COMMAND_TIMEOUT)
            return True
        return False

    def status(self, sequence: int, timestamp_us: int) -> Frame:
        payload = struct.pack(
            "<IIIIQQ",
            self.state,
            self.fault,
            self.last_rx_sequence,
            self.last_command_sequence,
            self.last_rx_timestamp_us,
            self.last_command_timestamp_us,
        )
        return Frame(STATUS, 0, sequence, timestamp_us, payload)


def self_test() -> None:
    gateway = AmpGateway()
    left, right = socket.socketpair()
    try:
        command_time = 1_000_000
        left.sendall(make_motion(1, command_time).pack())
        received = Frame.unpack(right.recv(FRAME_SIZE))
        gateway.accept(received, command_time)
        assert gateway.state == STATE_ACTIVE
        assert gateway.last_command_sequence == 1

        heartbeat_time = 1_050_000
        left.sendall(make_heartbeat(2, heartbeat_time).pack())
        gateway.accept(Frame.unpack(right.recv(FRAME_SIZE)), heartbeat_time)
        assert gateway.state == STATE_ACTIVE

        status = gateway.status(1, heartbeat_time)
        parsed_status = Frame.unpack(status.pack())
        assert parsed_status.message_type == STATUS
        assert struct.unpack("<I", parsed_status.payload[:4])[0] == STATE_ACTIVE

        assert gateway.tick(command_time + 300_000)
        assert gateway.state == STATE_EMERGENCY_STOP
        assert gateway.fault == FAULT_COMMAND_TIMEOUT
        assert gateway.command == (0.0, 0.0)
    finally:
        left.close()
        right.close()
    print("VelaROS AMP gateway self-test: PASS command=1 heartbeat=1 status=1 timeout_stop=1")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true", help="run the no-board protocol test")
    args = parser.parse_args()
    if not args.self_test:
        parser.error("only --self-test is available until K1 RPMsg/OpenAMP transport exists")
    self_test()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
