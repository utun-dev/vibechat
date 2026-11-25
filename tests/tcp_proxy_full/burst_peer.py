#!/usr/bin/env python3
"""Burst/load peer for tcp_proxy integration test (full-duplex, byte-exact).

Both sides stream a known pseudo-random byte sequence in random chunks
(10B..1MB) with optional random idle (100..500ms) between chunks, over full-duplex
TCP connections, and verify the received stream byte-for-byte.

Because the sequence is deterministic (SHAKE128 with a seed and frame counter),
every received byte is checked against a pre-known value — any corruption,
cross-connection mixing or stall is detected exactly.

Multiple concurrent connections are supported (--connections N); each connection
carries its index in a 4-byte handshake so both peers derive distinct per-connection
seeds and any cross-connection byte mixing is caught.

Roles:
  server:  burst_peer.py --server --host 127.0.0.1 --port 19193 --connections 8
  client:  burst_peer.py --host 10.200.100.1 --port 19193 --mark 1 --connections 8

Exit code: 0 = PASS (all connections byte-exact), 1 = FAIL.
"""

import argparse
import hashlib
import math
import random
import select
import signal
import socket
import struct
import sys
import threading
import time

SO_MARK = 36
MASK64 = (1 << 64) - 1


class ByteStream:
    """Generate distinct deterministic payloads for each connection, direction and frame."""

    def __init__(self, seed):
        self.seed = seed & MASK64
        self.frame = 0

    def next_bytes(self, n):
        payload = hashlib.shake_128(struct.pack(">QQ", self.seed, self.frame)).digest(n)
        self.frame += 1
        return payload


def rand_chunk(rnd, lo, hi):
    """Log-uniform chunk size in [lo, hi] — covers small (keep-alive) and large."""
    if hi <= lo:
        return lo
    v = math.exp(rnd.uniform(math.log(lo), math.log(hi)))
    return max(lo, min(hi, int(round(v))))


class StuckError(Exception):
    pass


def send_all(sock, data):
    view = memoryview(data)
    while len(view):
        n = sock.send(view)
        if n <= 0:
            raise RuntimeError("send failed (connection closed)")
        view = view[n:]


def recv_exact(sock, n):
    out = bytearray()
    while len(out) < n:
        c = sock.recv(n - len(out))
        if not c:
            raise RuntimeError("connection closed")
        out += c
    return bytes(out)


def receive_session(sock, recv_seed, total, stall_ms, name):
    """Read frames until END, verify byte-for-byte. Returns (bytes, frames)."""
    rng = ByteStream(recv_seed)
    received = 0
    nframes = 0
    stall_seconds = stall_ms / 1000.0
    last_progress = time.monotonic()
    last_log = time.monotonic()

    def read_n(n):
        nonlocal last_progress
        out = bytearray()
        while len(out) < n:
            timeout = last_progress + stall_seconds - time.monotonic()
            if timeout <= 0 or not select.select([sock], [], [], timeout)[0]:
                raise StuckError("no progress for %dms (received %d/%d)" % (stall_ms, received, total))
            c = sock.recv(n - len(out))
            if not c:
                raise RuntimeError("connection closed at %d/%d bytes" % (received, total))
            out += c
            last_progress = time.monotonic()
        return bytes(out)

    while True:
        hdr = read_n(4)
        (length,) = struct.unpack(">I", hdr)
        if length == 0:
            if received != total:
                raise RuntimeError("peer ended early: %d/%d" % (received, total))
            return received, nframes
        if received + length > total:
            raise RuntimeError("frame overflow: %d+%d > %d" % (received, length, total))
        payload = read_n(length)
        expected = rng.next_bytes(length)
        if payload != expected:
            off = 0
            for i in range(length):
                if payload[i] != expected[i]:
                    off = i
                    break
            raise RuntimeError("byte mismatch at offset %d: got %02x want %02x" %
                               (received + off, payload[off], expected[off]))
        received += length
        nframes += 1
        if time.monotonic() - last_log >= 1.0:
            print("[%s] recv %d/%d bytes" % (name, received, total), flush=True)
            last_log = time.monotonic()


def send_session(sock, send_seed, total, min_chunk, max_chunk, min_idle, max_idle):
    """Send `total` deterministic bytes in random chunks with random idle, then END."""
    rng = ByteStream(send_seed)
    rnd = random.Random(send_seed ^ 0xABCDEF)
    remaining = total
    nframes = 0
    while remaining > 0:
        length = rand_chunk(rnd, min_chunk, max_chunk)
        if length > remaining:
            length = remaining
        if length < 1:
            length = 1
        payload = rng.next_bytes(length)
        send_all(sock, struct.pack(">I", length) + payload)
        remaining -= length
        nframes += 1
        if remaining > 0:
            delay_ms = rnd.uniform(min_idle, max_idle)
            if delay_ms:
                time.sleep(delay_ms / 1000.0)
    send_all(sock, struct.pack(">I", 0))
    return nframes, total


def run_peer(sock, args, role, name, idx):
    """Run one full-duplex session. Returns (ok, recv_bytes, send_bytes, err)."""
    if role == "client":
        seed_out, seed_in = args.seed_c2s ^ idx, args.seed_s2c ^ idx
    else:
        seed_out, seed_in = args.seed_s2c ^ idx, args.seed_c2s ^ idx

    sender_res = {}

    def sender():
        try:
            sender_res["frames"], sender_res["bytes"] = send_session(
                sock, seed_out, args.total,
                args.min_chunk, args.max_chunk,
                args.min_idle_ms, args.max_idle_ms)
            sender_res["ok"] = True
        except Exception as e:
            sender_res["ok"] = False
            sender_res["err"] = str(e)

    t = threading.Thread(target=sender, daemon=True)
    t.start()

    try:
        recv, _ = receive_session(sock, seed_in, args.total, args.stall_ms, name)
    except StuckError as e:
        return False, 0, 0, "STUCK: %s" % e
    except Exception as e:
        return False, 0, 0, str(e)

    t.join()
    if not sender_res.get("ok"):
        return False, recv, 0, "send error: %s" % sender_res.get("err")

    return True, recv, sender_res["bytes"], None


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--server", action="store_true")
    p.add_argument("--host", required=True)
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--name", default="burst")
    p.add_argument("--connections", type=int, default=1)
    p.add_argument("--total", type=int, default=4 * 1024 * 1024)
    p.add_argument("--min-chunk", type=int, default=10)
    p.add_argument("--max-chunk", type=int, default=1048576)
    p.add_argument("--min-idle-ms", type=int, default=100)
    p.add_argument("--max-idle-ms", type=int, default=500)
    p.add_argument("--stall-ms", type=int, default=2000)
    p.add_argument("--mark", type=int, default=1)
    p.add_argument("--seed-c2s", type=lambda x: int(x, 0), default=0x11443322778855A)
    p.add_argument("--seed-s2c", type=lambda x: int(x, 0), default=0x2233445511223344)
    args = p.parse_args()

    if args.server:
        run_server(args)
    else:
        run_client(args)


def run_client(args):
    results = []
    lock = threading.Lock()
    t0 = time.monotonic()

    def worker(i):
        name = "%s[%d]" % (args.name, i)
        s = None
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            if args.mark:
                s.setsockopt(socket.SOL_SOCKET, SO_MARK, args.mark)
            s.connect((args.host, args.port))
            s.sendall(struct.pack(">I", i))
            ok, recv, send, err = run_peer(s, args, "client", name, i)
            if ok:
                print("[PASS] %s: recv %d sent %d" % (name, recv, send), flush=True)
            else:
                print("[FAIL] %s: %s" % (name, err), flush=True)
            with lock:
                results.append((ok, recv, send))
        except OSError as e:
            print("[FAIL] %s: connect: %s" % (name, e), flush=True)
            with lock:
                results.append((False, 0, 0))
        finally:
            if s:
                s.close()

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(args.connections)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    dt = time.monotonic() - t0
    ok = len(results) == args.connections and all(r[0] for r in results)
    total_bytes = sum(r[1] + r[2] for r in results)
    if ok:
        print("[PASS] %s: %d/%d connections, %d bytes in %.2fs (%.2f MB/s)" %
              (args.name, len(results), args.connections, total_bytes, dt,
               total_bytes / 1e6 / dt if dt > 0 else 0), flush=True)
        sys.exit(0)
    failed = args.connections - sum(1 for r in results if r[0])
    print("[FAIL] %s: %d/%d connections failed" % (args.name, failed, args.connections), flush=True)
    sys.exit(1)


def run_server(args):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((args.host, args.port))
    s.listen(256)
    print("BURST: listening on %s:%d connections=%d total=%d" %
          (args.host, args.port, args.connections, args.total), flush=True)

    results = []
    lock = threading.Lock()
    running = [True]

    def _stop(sig, frame):
        running[0] = False

    signal.signal(signal.SIGTERM, _stop)
    signal.signal(signal.SIGINT, _stop)

    def handle(conn):
        idx = 0
        try:
            idxb = recv_exact(conn, 4)
            (idx,) = struct.unpack(">I", idxb)
            name = "%s[%d]" % (args.name, idx)
            ok, recv, send, err = run_peer(conn, args, "server", name, idx)
            if ok:
                print("[PASS] %s: recv %d sent %d" % (name, recv, send), flush=True)
            else:
                print("[FAIL] %s: %s" % (name, err), flush=True)
            with lock:
                results.append(ok)
        except OSError as e:
            print("[FAIL] %s: %s" % (args.name, e), flush=True)
            with lock:
                results.append(False)
        finally:
            conn.close()

    s.settimeout(1.0)
    threads = []
    while running[0] and len(threads) < args.connections:
        try:
            conn, addr = s.accept()
        except socket.timeout:
            continue
        except OSError:
            break
        t = threading.Thread(target=handle, args=(conn,), daemon=True)
        t.start()
        threads.append(t)

    for t in threads:
        t.join()
    s.close()

    ok = len(results) == args.connections and all(results)
    if ok:
        print("[PASS] %s: %d/%d connections" % (args.name, len(results), args.connections), flush=True)
        sys.exit(0)
    print("[FAIL] %s: %d/%d connections ok" %
          (args.name, sum(1 for r in results if r), args.connections), flush=True)
    sys.exit(1)


if __name__ == "__main__":
    main()
