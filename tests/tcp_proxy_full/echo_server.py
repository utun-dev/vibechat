#!/usr/bin/env python3
"""TCP echo server for tcp_proxy integration test.

Accepts connections, reads all data, sends back.
Supports --read-timeout to close after idle (for half-close tests).
"""

import socket
import sys
import threading
import signal

running = True


def handle(conn: socket.socket, read_timeout: float):
    try:
        while True:
            if read_timeout > 0:
                conn.settimeout(read_timeout)
            try:
                data = conn.recv(65536)
            except socket.timeout:
                break
            if not data:
                break
            conn.sendall(data)
    except OSError:
        pass
    finally:
        conn.close()


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <host> <port> [--read-timeout SEC]", file=sys.stderr)
        sys.exit(1)

    host = sys.argv[1]
    port = int(sys.argv[2])
    read_timeout = 0.0
    for i in range(3, len(sys.argv)):
        if sys.argv[i] == "--read-timeout" and i + 1 < len(sys.argv):
            read_timeout = float(sys.argv[i + 1])

    global running

    def _sigterm(sig, frame):
        global running
        running = False

    signal.signal(signal.SIGTERM, _sigterm)
    signal.signal(signal.SIGINT, _sigterm)

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((host, port))
    s.listen(128)
    print(f"ECHO: listening on {host}:{port}" + (f" timeout={read_timeout}s" if read_timeout > 0 else ""), flush=True)

    while running:
        try:
            s.settimeout(1.0)
            conn, addr = s.accept()
        except socket.timeout:
            continue
        except OSError:
            break
        t = threading.Thread(target=handle, args=(conn, read_timeout), daemon=True)
        t.start()

    s.close()


if __name__ == "__main__":
    main()
