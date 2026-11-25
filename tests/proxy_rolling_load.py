"""Maintain 64 simultaneous TCP proxy streams against a loopback echo server."""

import asyncio
import socket
import sys
import time


CONCURRENCY = 64


async def echo(reader, writer):
    try:
        while True:
            data = await reader.read(16 * 1024)
            if not data:
                return
            writer.write(data)
            await writer.drain()
    finally:
        writer.close()
        await writer.wait_closed()


async def handshake(reader, writer, mode, target, refused=False):
    if mode == "SOCKS":
        writer.write(b"\x05\x01\x00")
        if await reader.readexactly(2) != b"\x05\x00":
            raise RuntimeError("SOCKS greeting rejected")
        writer.write(b"\x05\x01\x00\x01\x7f\x00\x00\x01" + target.to_bytes(2, "big"))
        reply = await reader.readexactly(10)
        if reply[:2] != (b"\x05\x04" if refused else b"\x05\x00"):
            raise RuntimeError(f"SOCKS CONNECT reply {reply.hex()}")
    elif mode != "TUN":
        address = f"127.0.0.1:{target}"
        writer.write(f"CONNECT {address} HTTP/1.1\r\nHost: {address}\r\n\r\n".encode())
        reply = await reader.readuntil(b"\r\n\r\n")
        if reply.split(b" ", 2)[1:2] != [b"502" if refused else b"200"]:
            raise RuntimeError(f"HTTP CONNECT reply {reply!r}")


async def connect(host, port, marked):
    if not marked:
        return await asyncio.open_connection(host, port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_MARK, 1)
        sock.setblocking(False)
        await asyncio.get_running_loop().sock_connect(sock, (host, port))
        return await asyncio.open_connection(sock=sock)
    except BaseException:
        sock.close()
        raise


async def run_round(mode, proxy_port, target, seconds, host="127.0.0.1", marked=False):
    first_wave = asyncio.Event()
    established = 0
    finish = 0

    async def worker(slot):
        nonlocal established, finish
        completed = 0
        while not completed or time.monotonic() < finish:
            async def exchange():
                nonlocal established, finish
                reader, writer = await connect(host, proxy_port, marked)
                try:
                    await handshake(reader, writer, mode, target)
                    if not completed:
                        established += 1
                        if established == CONCURRENCY:
                            finish = time.monotonic() + seconds
                            first_wave.set()
                        await first_wave.wait()
                    payload = bytes((slot, completed % 256)) * (16 * 1024)
                    writer.write(payload)
                    await writer.drain()
                    if await reader.readexactly(len(payload)) != payload:
                        raise RuntimeError("echo payload mismatch")
                    writer.write_eof()
                    if await reader.read(1) != b"":
                        raise RuntimeError("unexpected data after echo")
                finally:
                    writer.close()
                    await writer.wait_closed()

            try:
                await asyncio.wait_for(exchange(), 15)
            except Exception as error:
                raise RuntimeError(f"{mode} slot {slot}, completed={completed}: {error!r}") from error
            completed += 1
        return completed

    counts = await asyncio.gather(*(worker(slot) for slot in range(CONCURRENCY)))
    if min(counts) < 2:
        raise RuntimeError(f"{mode} did not replenish all slots: {counts}")
    print(f"[PASS] {mode} {CONCURRENCY} simultaneous, {seconds}s, {sum(counts)} complete, min={min(counts)} per slot", flush=True)


async def main(socks, http, seconds):
    server = await asyncio.start_server(echo, "127.0.0.1", 0, backlog=128, family=socket.AF_INET)
    async with server:
        target = server.sockets[0].getsockname()[1]
        stop = asyncio.Event()

        async def background(mode, port, ready):
            reader, writer = await connect("127.0.0.1", port, False)
            completed = 0
            try:
                await handshake(reader, writer, mode, target)
                ready.set()
                payload = bytes(range(256)) * 256
                while not stop.is_set():
                    writer.write(payload)
                    await writer.drain()
                    if await reader.readexactly(len(payload)) != payload:
                        raise RuntimeError(f"{mode} background payload mismatch")
                    completed += 1
            finally:
                writer.close()
                await writer.wait_closed()
            return completed

        ready = [asyncio.Event(), asyncio.Event()]
        workers = [asyncio.create_task(background(mode, port, event))
                   for (mode, port), event in zip((("SOCKS", socks), ("HTTP CONNECT", http)), ready)]
        try:
            await asyncio.wait_for(asyncio.gather(*(event.wait() for event in ready)), 15)
            await run_round("SOCKS", socks, target, seconds)
            await run_round("HTTP CONNECT", http, target, seconds)
        finally:
            stop.set()
            counts = await asyncio.wait_for(asyncio.gather(*workers), 15)
        if min(counts) < 2:
            raise RuntimeError(f"background connections stopped making progress: {counts}")
        print(f"[PASS] background SOCKS and HTTP CONNECT: {counts} byte-exact blocks", flush=True)

    async def check_refused(mode, port, closed_port):
        reader, writer = await connect("127.0.0.1", port, False)
        try:
            await handshake(reader, writer, mode, closed_port, refused=True)
            if await reader.read(1) != b"":
                raise RuntimeError(f"{mode} sent data after refusing CONNECT")
        finally:
            writer.close()
            await writer.wait_closed()

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as closed:
        closed.bind(("127.0.0.1", 0))
        for mode, port in (("SOCKS", socks), ("HTTP CONNECT", http)):
            await asyncio.wait_for(check_refused(mode, port, closed.getsockname()[1]), 10)
    print("[PASS] SOCKS and HTTP CONNECT report a real exit TCP refusal and close", flush=True)


async def tun_main(host, port, seconds):
    server = await asyncio.start_server(echo, "127.0.0.1", port, backlog=128, family=socket.AF_INET)
    async with server:
        await run_round("TUN", port, port, seconds, host, True)

    async def check_reset():
        try:
            reader, writer = await connect(host, port, True)
            try:
                await reader.read(1)
            finally:
                writer.close()
                await writer.wait_closed()
        except ConnectionResetError:
            return
        raise RuntimeError("TUN upstream refusal did not reset the local TCP connection")

    await asyncio.wait_for(check_reset(), 10)
    print("[PASS] TUN upstream refusal resets local TCP", flush=True)


if __name__ == "__main__":
    if sys.argv[1] == "--tun":
        asyncio.run(tun_main(sys.argv[2], int(sys.argv[3]), int(sys.argv[4])))
    else:
        asyncio.run(main(*(int(value) for value in sys.argv[1:])))
