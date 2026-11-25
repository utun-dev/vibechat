"""UDP-калибровка из настоящих namespace, до запуска ETCP."""

import argparse
import json
from pathlib import Path
import select
import socket
import statistics
import sys
import time
import uuid

from topology import NATS, NODES, WANS


def serve(address):
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind((address, 32000))
    udp.setblocking(False)
    print(json.dumps({"ready": address}), flush=True)

    def receive():
        data, peer = udp.recvfrom(4096)
        message = json.loads(data)
        if message.get("request"):
            udp.sendto(json.dumps({"token": message["token"], "observed": peer}).encode(), peer)
            return None
        return message

    while True:
        readable, _, _ = select.select([sys.stdin, udp], [], [])
        if udp in readable:
            receive()
        if sys.stdin not in readable:
            continue
        line = sys.stdin.readline()
        if not line:
            break
        command = json.loads(line)
        if command.get("quit"):
            break
        samples, observed, lost = [], None, 0
        for _ in range(command.get("count", 1)):
            token = uuid.uuid4().hex
            packet = json.dumps({"request": True, "token": token}).encode()
            started = time.monotonic()
            udp.sendto(packet, (command["ip"], command["port"]))
            deadline = started + command.get("timeout", 0.8)
            while time.monotonic() < deadline:
                if not select.select([udp], [], [], max(0, deadline - time.monotonic()))[0]:
                    break
                reply = receive()
                if reply and reply.get("token") == token:
                    samples.append((time.monotonic() - started) * 1000)
                    observed = reply["observed"]
                    break
            if len(samples) + lost < _ + 1:
                lost += 1
        print(json.dumps({"samples_ms": samples, "observed": observed, "lost": lost}), flush=True)
    udp.close()


class Probes:
    def __init__(self, network):
        self.network = network
        self.processes = {}

    def start(self):
        for node in NODES:
            process = self.network.spawn(node.name, [sys.executable, "-u", str(Path(__file__).resolve()), "--serve", node.ip],
                                         stdin=-1, stdout=-1, text=True)
            if not select.select([process.stdout], [], [], 5)[0]:
                raise RuntimeError(f"Probe {node.name} failed to start")
            ready = json.loads(process.stdout.readline())
            if ready != {"ready": node.ip}:
                raise RuntimeError(f"Unexpected probe handshake: {ready}")
            self.processes[node.name] = process

    def exchange(self, name, ip, port=32000, count=3, timeout=0.8):
        process = self.processes[name]
        process.stdin.write(json.dumps({"ip": ip, "port": port, "count": count, "timeout": timeout}) + "\n")
        process.stdin.flush()
        if not select.select([process.stdout], [], [], count * timeout + 5)[0]:
            raise RuntimeError(f"Probe {name} command timed out")
        line = process.stdout.readline()
        if not line:
            raise RuntimeError(f"Probe {name} exited: {process.poll()}")
        return json.loads(line)

    def calibrate(self):
        results = {"lan": [], "wan": [], "nat": []}
        public = {n.country: n for n in NODES if not n.nat and n.client_type == 0}
        for country, first in public.items():
            second = next(n for n in NODES if n.country == country and not n.nat and n.name != first.name)
            result = self.exchange(first.name, second.ip, count=20)
            if len(result["samples_ms"]) < 19 or statistics.median(result["samples_ms"]) > 10:
                raise RuntimeError(f"LAN {country}: measured connection is not a good local link: {result}")
            results["lan"].append(dict(result, country=country, median_ms=statistics.median(result["samples_ms"])))
        for link in WANS:
            result = self.exchange(public[link.a].name, public[link.b].ip, count=40)
            if len(result["samples_ms"]) < 30:
                raise RuntimeError(f"WAN {link.a}-{link.b}: excessive loss: {result}")
            median = statistics.median(result["samples_ms"])
            if abs(median - link.rtt_ms) > max(12, link.rtt_ms * 0.25):
                raise RuntimeError(f"WAN {link.a}-{link.b}: measured RTT {median:.1f}ms, expected {link.rtt_ms}ms")
            results["wan"].append(dict(result, a=link.a, b=link.b, expected_rtt_ms=link.rtt_ms, median_ms=median))
        for gateway in NATS:
            for node in (n for n in NODES if n.nat == gateway.name):
                a, b, c = public["a"], public["b"], public["c"]
                outsider = next(n for n in NODES if n.country == "c" and not n.nat and n.name != c.name)
                before = self.exchange(outsider.name, gateway.ip, node.mapped_port, count=1, timeout=0.35)
                if before["observed"]:
                    raise RuntimeError(f"NAT {node.name}: incoming mapping exists before outbound traffic")
                first = self.exchange(node.name, a.ip)
                second = self.exchange(node.name, b.ip)
                if not first["observed"] or not second["observed"]:
                    raise RuntimeError(f"NAT {node.name}: outgoing UDP failed: {first}, {second}")
                stable = first["observed"] == second["observed"]
                third = self.exchange(c.name, gateway.ip, first["observed"][1], count=3, timeout=0.4)
                inbound = bool(third["observed"])
                expected = (gateway.mode != "strict", gateway.mode == "cone")
                if (stable, inbound) != expected:
                    raise RuntimeError(f"NAT {node.name}/{gateway.mode}: stable={stable}, unsolicited={inbound}, expected={expected}")
                opened = self.exchange(node.name, c.ip, count=3)
                if not opened["observed"]:
                    raise RuntimeError(f"NAT {node.name}/{gateway.mode}: outgoing to previous unsolicited sender failed: {opened}")
                after = self.exchange(c.name, gateway.ip, opened["observed"][1], count=3, timeout=0.4)
                if not after["observed"]:
                    raise RuntimeError(f"NAT {node.name}/{gateway.mode}: incoming failed after contact: {after}")
                results["nat"].append({"node": node.name, "gateway": gateway.name, "mode": gateway.mode,
                                       "first": first["observed"], "second": second["observed"],
                                       "stable": stable, "unsolicited": inbound, "after_contact": bool(after["observed"])})
        return results

    def close(self):
        for process in self.processes.values():
            if process.poll() is None:
                process.stdin.close()
        for process in self.processes.values():
            try:
                process.wait(timeout=3)
            except Exception:
                process.kill()
                process.wait()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--serve", required=True)
    serve(parser.parse_args().serve)
