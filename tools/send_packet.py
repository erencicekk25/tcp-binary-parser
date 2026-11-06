#!/usr/bin/env python3
"""Minimal test client that sends SensorPayload packets to tcp_binary_parser."""

import argparse
import random
import socket
import struct
import sys
import time

SYNC_BYTE = 0xAA
CMD_SENSOR_DATA = 0x01
PAYLOAD_FORMAT = "<Iff"  # timestamp (uint32), temperature (float32), humidity (float32)
HEADER_FORMAT = "<BBH"   # sync_byte, cmd_id, payload_len


def build_packet(timestamp: int, temperature: float, humidity: float) -> bytes:
    payload = struct.pack(PAYLOAD_FORMAT, timestamp, temperature, humidity)
    header = struct.pack(HEADER_FORMAT, SYNC_BYTE, CMD_SENSOR_DATA, len(payload))
    return header + payload


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--count", type=int, default=5, help="number of packets to send")
    parser.add_argument("--interval", type=float, default=0.5, help="seconds between packets")
    parser.add_argument("--fragment", action="store_true",
                        help="send each packet byte by byte to exercise stream reassembly")
    parser.add_argument("--noise", action="store_true",
                        help="prepend random garbage bytes to exercise resynchronization")
    args = parser.parse_args()

    try:
        with socket.create_connection((args.host, args.port), timeout=5) as sock:
            for _ in range(args.count):
                packet = build_packet(
                    int(time.time()),
                    round(random.uniform(18.0, 30.0), 2),
                    round(random.uniform(30.0, 70.0), 2),
                )
                if args.noise:
                    packet = bytes([0x00, 0x13, 0x37]) + packet

                if args.fragment:
                    for byte in packet:
                        sock.sendall(bytes([byte]))
                        time.sleep(0.01)
                else:
                    sock.sendall(packet)

                print(f"sent {len(packet)} bytes: {packet.hex(' ')}")
                time.sleep(args.interval)
    except OSError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
