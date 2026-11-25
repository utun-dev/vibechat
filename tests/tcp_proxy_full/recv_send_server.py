#!/usr/bin/env python3
"""TCP server for tcp_proxy integration test: recv 1MB → send 1MB.

Each connection receives exactly 1MB fully, then sends response:
[sentinel(4) || received_data(1MB)] = 1048580 bytes total.
"""

import socket
import struct
import sys
import threading
import signal

running = True
conn_seq = 0
lock = threading.Lock()

ONE_MB = 1048576
SENTINEL = 0xBEEF0102


def recv_exact(conn: socket.socket, n: int) -> bytes:
    data = bytearray()
    while len(data) < n:
        chunk = conn.recv(min(65536, n - len(data)))
        if not chunk:
            break
        data += chunk
    return bytes(data)


def send_exact(conn: socket.socket, data: bytes) -> None:
    sent = 0
    while sent < len(data):
        n = conn.send(data[sent:])
        if n <= 0:
            raise OSError("send failed")
        sent += n


def handle(conn: socket.socket):
    global conn_seq
    with lock:
        conn_seq += 1
        seq = conn_seq
    try:
        received = recv_exact(conn, ONE_MB)
        print(f"SERVER conn={seq} recv={len(received)}", flush=True)
        if len(received) == ONE_MB:
            resp = struct.pack(">I", SENTINEL) + received
            send_exact(conn, resp)
            print(f"SERVER conn={seq} sent={len(resp)}", flush=True)
        else:
            print(f"SERVER conn={seq} short_recv={len(received)}/{ONE_MB}", flush=True)
    except OSError as e:
        print(f"SERVER conn={seq} error: {e}", flush=True)
    finally:
        conn.close()


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <host> <port>", file=sys.stderr)
        sys.exit(1)

    host = sys.argv[1]
    port = int(sys.argv[2])

    global running

    def _stop(sig, frame):
        global running
        running = False

    signal.signal(signal.SIGTERM, _stop)
    signal.signal(signal.SIGINT, _stop)

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((host, port))
    s.listen(128)
    print(f"SERVER: listening on {host}:{port}", flush=True)

    while running:
        try:
            s.settimeout(1.0)
            conn, addr = s.accept()
        except socket.timeout:
            continue
        except OSError:
            break
        t = threading.Thread(target=handle, args=(conn,), daemon=True)
        t.start()

    s.close()


if __name__ == "__main__":
    main()
