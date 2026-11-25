"""RFC5780-сервер стенда: два процесса, два реальных IP, по два UDP-порта."""

import argparse
import ipaddress
import json
import select
import socket
import struct
import zlib

COOKIE = 0x2112A442
PORTS = (3478, 3479)
CONTROL = 39000


def address(kind, endpoint):
    ip, port = endpoint
    mask = COOKIE if kind == 0x0020 else 0
    return struct.pack("!HHBBHI", kind, 8, 0, 1, port ^ (mask >> 16), int(ipaddress.IPv4Address(ip)) ^ mask)


def response(transaction, mapped, origin, other):
    attributes = address(0x0020, mapped) + address(0x802B, origin) + address(0x802C, other)
    packet = struct.pack("!HHI12s", 0x0101, len(attributes) + 8, COOKIE, transaction) + attributes
    return packet + struct.pack("!HHI", 0x8028, 4, zlib.crc32(packet) ^ 0x5354554E)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ip", required=True)
    parser.add_argument("--peer", required=True)
    args = parser.parse_args()
    sockets = {}
    for port in (*PORTS, CONTROL):
        endpoint = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        endpoint.bind((args.ip, port))
        sockets[endpoint] = port
    print("READY", flush=True)
    try:
        while True:
            readable, _, _ = select.select(sockets, [], [], 1)
            for endpoint in readable:
                packet, client = endpoint.recvfrom(2048)
                port = sockets[endpoint]
                if port == CONTROL:
                    if client != (args.peer, CONTROL):
                        continue
                    command = json.loads(packet)
                    sender = next(sock for sock, number in sockets.items() if number == command["port"])
                    reply = response(bytes.fromhex(command["transaction"]), command["mapped"],
                                     (args.ip, command["port"]), command["other"])
                    sender.sendto(reply, tuple(command["client"]))
                    continue
                if len(packet) < 20:
                    continue
                kind, size, cookie, transaction = struct.unpack("!HHI12s", packet[:20])
                if kind != 1 or cookie != COOKIE or size + 20 != len(packet):
                    continue
                flags, offset = 0, 20
                while offset + 4 <= len(packet):
                    attribute, length = struct.unpack("!HH", packet[offset:offset + 4])
                    if offset + 4 + length > len(packet):
                        raise ValueError("malformed STUN attribute")
                    if attribute == 0x0003 and length == 4:
                        flags = struct.unpack("!I", packet[offset + 4:offset + 8])[0]
                    offset += 4 + ((length + 3) & ~3)
                other = (args.peer, PORTS[1] if port == PORTS[0] else PORTS[0])
                reply_port = other[1] if flags & 2 else port
                if flags & 4:
                    command = {"transaction": transaction.hex(), "mapped": client, "client": client,
                               "port": reply_port, "other": other}
                    control = next(sock for sock, number in sockets.items() if number == CONTROL)
                    control.sendto(json.dumps(command).encode(), (args.peer, CONTROL))
                else:
                    sender = next(sock for sock, number in sockets.items() if number == reply_port)
                    sender.sendto(response(transaction, client, (args.ip, reply_port), other), client)
                print(json.dumps({"client": client, "port": port, "change": flags,
                                  "transaction": transaction.hex()}), flush=True)
    finally:
        for endpoint in sockets:
            endpoint.close()


if __name__ == "__main__":
    main()
