#!/usr/bin/env python3
"""TCP test client for tcp_proxy recv→send integration test.

Connects to host:port with SO_MARK=1, sends 1MB random data,
receives 4-byte sentinel + 1MB response, verifies.
Supports --count N for multiple sequential requests.
"""

import os
import socket
import struct
import sys
import time

SO_MARK = 36
ONE_MB = 1048576
SENTINEL = 0xBEEF0102


def recv_exact(sock: socket.socket, n: int) -> bytes:
    data = bytearray()
    while len(data) < n:
        chunk = sock.recv(min(65536, n - len(data)))
        if not chunk:
            break
        data += chunk
    return bytes(data)


def send_exact(sock: socket.socket, data: bytes) -> None:
    sent = 0
    while sent < len(data):
        n = sock.send(data[sent:])
        if n <= 0:
            raise OSError("send failed")
        sent += n


def run_one(args, payload: bytes, idx: int) -> int:
    t0 = time.monotonic()
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    if args.mark != 0:
        s.setsockopt(socket.SOL_SOCKET, SO_MARK, args.mark)
    s.settimeout(args.timeout)

    try:
        s.connect((args.host, args.port))
    except OSError as e:
        print(f"[FAIL] {args.name}[{idx}]: connect: {e}", flush=True)
        s.close()
        return 1

    try:
        send_exact(s, payload)
    except OSError as e:
        print(f"[FAIL] {args.name}[{idx}]: send: {e}", flush=True)
        s.close()
        return 1

    try:
        got = recv_exact(s, 4 + args.size)
    except OSError as e:
        print(f"[FAIL] {args.name}[{idx}]: recv: {e}", flush=True)
        s.close()
        return 1

    s.close()
    elapsed = time.monotonic() - t0

    if len(got) != 4 + args.size:
        print(f"[FAIL] {args.name}[{idx}]: len={len(got)}/{4 + args.size} in {elapsed:.3f}s", flush=True)
        return 1

    sentinel, = struct.unpack(">I", got[:4])
    if sentinel != SENTINEL:
        print(f"[FAIL] {args.name}[{idx}]: sentinel={sentinel:#010x} expected={SENTINEL:#010x}", flush=True)
        return 1

    received = got[4:]
    if not args.verify:
        throughput = (args.size * 2) / 1e6 / elapsed
        print(f"[PASS] {args.name}[{idx}]: {args.size} bytes in {elapsed:.3f}s ({throughput:.2f} MB/s)", flush=True)
        return 0

    if payload != received:
        for i in range(min(len(payload), len(received))):
            if payload[i] != received[i]:
                print(f"[FAIL] {args.name}[{idx}]: mismatch at {i}: "
                      f"sent={payload[i]:02x} recv={received[i]:02x}", flush=True)
                return 1
        print(f"[FAIL] {args.name}[{idx}]: size mismatch", flush=True)
        return 1

    throughput = (args.size * 2) / 1e6 / elapsed
    print(f"[PASS] {args.name}[{idx}]: {args.size} bytes in {elapsed:.3f}s ({throughput:.2f} MB/s)", flush=True)
    return 0


def main():
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--size", type=int, default=ONE_MB)
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--mark", type=int, default=1, help="SO_MARK (0=off)")
    parser.add_argument("--count", type=int, default=1, help="number of sequential requests")
    parser.add_argument("--name", default="test")
    parser.add_argument("--seed", type=int, default=0, help="random seed")
    args = parser.parse_args()

    import random
    if args.seed:
        random.seed(args.seed)

    payloads = [os.urandom(args.size) for _ in range(args.count)]
    fails = 0

    for i in range(args.count):
        if run_one(args, payloads[i], i + 1) != 0:
            fails += 1
        time.sleep(0.1)

    if fails > 0:
        print(f"[FAIL] {args.name}: {fails}/{args.count} failed", flush=True)
        sys.exit(1)
    print(f"[PASS] {args.name}: all {args.count} ok", flush=True)


if __name__ == "__main__":
    main()
