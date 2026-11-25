#!/usr/bin/env python3
"""TCP test client for tcp_proxy integration test.

Connects to host:port with SO_MARK=1, sends N bytes, receives echo, verifies.
"""

import os
import socket
import sys
import time

SO_MARK = 36


def main():
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--size", type=int, required=True)
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--half-close", action="store_true")
    parser.add_argument("--sleep-before", type=float, default=0)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--mark", type=int, default=1, help="SO_MARK (0=off)")
    parser.add_argument("--name", default="test")
    args = parser.parse_args()

    payload = os.urandom(args.size)
    t0 = time.monotonic()

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    if args.mark != 0:
        s.setsockopt(socket.SOL_SOCKET, SO_MARK, args.mark)
    s.settimeout(args.timeout)

    try:
        s.connect((args.host, args.port))
    except OSError as e:
        print(f"[FAIL] {args.name}: connect failed: {e}", flush=True)
        s.close()
        sys.exit(1)

    if args.sleep_before > 0:
        time.sleep(args.sleep_before)

    try:
        s.sendall(payload)
    except OSError as e:
        print(f"[FAIL] {args.name}: sendall failed: {e}", flush=True)
        s.close()
        sys.exit(1)

    if args.half_close:
        s.shutdown(socket.SHUT_WR)

    if not args.verify and not args.half_close:
        elapsed = time.monotonic() - t0
        print(f"[PASS] {args.name}: {args.size} bytes sent in {elapsed:.3f}s "
              f"({args.size / 1e6 / elapsed:.2f} MB/s)", flush=True)
        s.close()
        sys.exit(0)

    received = bytearray()
    expected = args.size
    try:
        while len(received) < expected:
            chunk = s.recv(min(65536, expected - len(received)))
            if not chunk:
                break
            received += chunk
    except OSError as e:
        print(f"[FAIL] {args.name}: recv error after {len(received)} bytes: {e}", flush=True)
        s.close()
        sys.exit(1)

    s.close()
    elapsed = time.monotonic() - t0

    if len(received) != expected:
        print(f"[FAIL] {args.name}: {len(received)}/{expected} bytes in {elapsed:.3f}s", flush=True)
        sys.exit(1)

    if payload != received:
        for i in range(min(len(payload), len(received))):
            if payload[i] != received[i]:
                print(f"[FAIL] {args.name}: data mismatch at byte {i}: "
                      f"sent={payload[i]:02x} recv={received[i]:02x}", flush=True)
                sys.exit(1)

    throughput = (expected * 2) / 1e6 / elapsed
    print(f"[PASS] {args.name}: {expected} bytes echoed in {elapsed:.3f}s ({throughput:.2f} MB/s)", flush=True)


if __name__ == "__main__":
    main()
