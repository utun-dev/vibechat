#!/usr/bin/env python3
"""Stress test: parallel threads, sequential echo requests, data verification."""

import os
import socket
import sys
import threading
import random
import time

SO_MARK = 36


def worker(thread_id, host, ports, iters, min_size, max_size, timeout, verbose, results_raw):
    ok = 0
    times = []
    for i in range(iters):
        size = random.randint(min_size, max_size)
        port = ports[i % len(ports)]
        payload = os.urandom(size)

        t0 = time.monotonic()
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, SO_MARK, 1)
        s.settimeout(timeout)
        try:
            t1 = time.monotonic()
            s.connect((host, port))
            t2 = time.monotonic()
            s.sendall(payload)
            t3 = time.monotonic()

            received = bytearray()
            while len(received) < size:
                chunk = s.recv(min(65536, size - len(received)))
                if not chunk:
                    break
                received += chunk
            t4 = time.monotonic()

            if len(received) != size or received != payload:
                raise RuntimeError(f"data mismatch")
            ok += 1
            times.append((size, int((t2-t1)*1000), int((t3-t2)*1000), int((t4-t3)*1000)))
        except Exception as e:
            results_raw[thread_id] = (False, f"FAIL iter={i} size={size}: {e} "
                                      f"connect={int((t2-t1)*1000)}ms send={int((t3-t2)*1000)}ms recv={int((t4-t3)*1000)}ms")
            try:
                s.close()
            except Exception:
                pass
            return
        finally:
            if s:
                try:
                    s.close()
                except Exception:
                    pass

    agg_connect = sum(t[1] for t in times)
    agg_send = sum(t[2] for t in times)
    agg_recv = sum(t[3] for t in times)
    total_bytes = sum(t[0] for t in times)
    results_raw[thread_id] = (True, f"PASS {ok}/{iters} c={agg_connect}ms s={agg_send}ms r={agg_recv}ms total={total_bytes}B")

    if verbose:
        for i, (sz, cm, sm, rm) in enumerate(times):
            print(f"  TH{thread_id:02d} iter={i:02d} size={sz:6d} c={cm:4d}ms s={sm:4d}ms r={rm:4d}ms", flush=True)


def main():
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--port-base", type=int, required=True)
    parser.add_argument("--ports", type=int, default=20)
    parser.add_argument("--threads", type=int, default=10)
    parser.add_argument("--iters", type=int, default=20)
    parser.add_argument("--min-size", type=int, default=1024)
    parser.add_argument("--max-size", type=int, default=1048576)
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    ports = [args.port_base + i for i in range(args.ports)]
    random.seed(os.urandom(8))

    threads = []
    results_raw = [None] * args.threads

    t0 = time.monotonic()

    for t in range(args.threads):
        th = threading.Thread(
            target=worker,
            args=(t, args.host, ports, args.iters, args.min_size, args.max_size, args.timeout, args.verbose, results_raw),
            daemon=True,
        )
        th.start()
        threads.append(th)

    for th in threads:
        th.join()

    elapsed = time.monotonic() - t0

    passed = sum(1 for r in results_raw if r and r[0])
    total_iters = args.threads * args.iters

    print(f"[{'PASS' if passed == args.threads else 'FAIL'}] stress: "
          f"{passed}/{args.threads} threads ({total_iters} sessions) in {elapsed:.1f}s", flush=True)

    for i, r in enumerate(results_raw):
        if r:
            print(f"    thread {i}: {r[1]}", flush=True)
    if passed < args.threads:
        sys.exit(1)


if __name__ == "__main__":
    main()
