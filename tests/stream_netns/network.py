"""Сетевые операции допускаются только в namespace, созданных этим запуском."""

import json
import os
from pathlib import Path
import select
import subprocess
import time

from topology import COUNTRIES, NATS, NODES, WANS, wan


def nat_rules(gateway, stun_trial=False, extra_destinations=()):
    clients = [node for node in NODES if node.nat == gateway.name]
    destinations = sorted({n.ip for n in NODES if not n.nat} | {g.ip for g in NATS} | set(extra_destinations))
    lines = ["table ip lab {",
             " set active { type inet_service; flags dynamic,timeout; timeout 60s; }",
             " set contacts { type ipv4_addr . inet_service . inet_service; flags dynamic,timeout; timeout 60s; }",
             " chain refresh { type filter hook postrouting priority mangle; policy accept;"]
    for node in clients:
        match = f' oifname "wan" ip saddr {node.ip} udp sport {node.udp_port}'
        if gateway.mode == "cone":
            lines.append(f"{match} update @active {{ {node.mapped_port} timeout 60s }}")
        elif gateway.mode == "restricted":
            lines.append(f"{match} update @contacts {{ ip daddr . udp dport . {node.mapped_port} timeout 60s }}")
        elif gateway.mode == "strict":
            for index, destination in enumerate(destinations):
                port = 40000 + node.number * 100 + index
                lines.append(f"{match} ip daddr {destination} update @contacts {{ ip daddr . udp dport . {port} timeout 60s }}")
    # STUN-сценарий использует одного клиента шлюза. Его случайные тестовые порты
    # получают тот же класс NAT, что рабочий 32000; внешние порты остаются другими.
    trial = clients[0]
    if stun_trial:
        match = f' oifname "wan" ip saddr {trial.ip} udp sport 49152-65535'
        if gateway.mode == "cone":
            lines.append(f"{match} update @active {{ udp sport timeout 60s }}")
        elif gateway.mode == "restricted":
            lines.append(f"{match} update @contacts {{ ip daddr . udp dport . udp sport timeout 60s }}")
        else:
            for index, destination in enumerate(destinations):
                port = 40000 + trial.number * 100 + index
                lines.append(f"{match} ip daddr {destination} update @contacts {{ ip daddr . udp dport . {port} timeout 60s }}")
    # Запрещённый вход отсекается ДО conntrack. Иначе первая неуспешная
    # входящая проба закрепляет tuple без DNAT и блокирует последующее открытие.
    lines += [" }", " chain ingress_guard { type filter hook prerouting priority raw; policy accept;"]
    for node in clients:
        ports = ([40000 + node.number * 100 + index for index in range(len(destinations))]
                 if gateway.mode == "strict" else [node.mapped_port])
        for port in ports:
            match = f' iifname "wan" udp dport {port}'
            if gateway.mode == "cone":
                lines.append(f"{match} udp dport @active accept")
            else:
                lines.append(f"{match} ip saddr . udp sport . udp dport @contacts accept")
            lines.append(f"{match} counter drop")
    if stun_trial and gateway.mode != "strict":
        match = ' iifname "wan" udp dport 49152-65535'
        guard = "udp dport @active" if gateway.mode == "cone" else "ip saddr . udp sport . udp dport @contacts"
        lines += [f"{match} {guard} accept", f"{match} counter drop"]
    lines += [" }", " chain prerouting { type nat hook prerouting priority dstnat; policy accept;"]
    for node in clients:
        match = f' iifname "wan" udp dport {node.mapped_port}'
        if gateway.mode == "cone":
            lines.append(f"{match} udp dport @active dnat to {node.ip}:{node.udp_port}")
        elif gateway.mode == "restricted":
            lines.append(f"{match} ip saddr . udp sport . udp dport @contacts dnat to {node.ip}:{node.udp_port}")
    if stun_trial and gateway.mode != "strict":
        guard = "udp dport @active" if gateway.mode == "cone" else "ip saddr . udp sport . udp dport @contacts"
        lines.append(f' iifname "wan" udp dport 49152-65535 {guard} dnat to {trial.ip}')
    lines += [" }", " chain postrouting { type nat hook postrouting priority srcnat; policy accept;"]
    for node in clients:
        match = f' oifname "wan" ip saddr {node.ip} udp sport {node.udp_port}'
        if gateway.mode == "strict":
            for index, destination in enumerate(destinations):
                port = 40000 + node.number * 100 + index
                lines.append(f"{match} ip daddr {destination} snat to {gateway.ip}:{port}")
        else:
            lines.append(f"{match} snat to {gateway.ip}:{node.mapped_port}")
    if stun_trial:
        match = f' oifname "wan" ip saddr {trial.ip} udp sport 49152-65535'
        if gateway.mode == "strict":
            for index, destination in enumerate(destinations):
                port = 40000 + trial.number * 100 + index
                lines.append(f"{match} ip daddr {destination} snat to {gateway.ip}:{port}")
        else:
            lines.append(f"{match} snat to {gateway.ip}")
    lines += [' oifname "wan" masquerade', " }",
              " chain forward { type filter hook forward priority filter; policy drop;",
              " ct state established,related accept", ' iifname "lan" oifname "wan" accept',
              " ct status dnat accept", " counter drop", " }", "}"]
    return "\n".join(lines) + "\n"


class Network:
    def __init__(self, directory, seed, host_namespace):
        self.directory = Path(directory)
        self.seed = seed
        self.host_namespace = host_namespace
        self.controller = os.readlink("/proc/self/ns/net")
        if self.controller == host_namespace or not host_namespace:
            raise RuntimeError("Refusing network changes: controller must be in a new network namespace")
        self.namespaces = {}
        self.children = []
        self.serial = 0
        self.log = (self.directory / "network.jsonl").open("a", buffering=1)

    def command(self, name, arguments, *, input=None, check=True):
        prefix = self.prefix(name)
        started = time.monotonic()
        result = subprocess.run(prefix + list(arguments), input=input, text=True, capture_output=True, timeout=20)
        inode = self.namespaces[name][1] if name else self.controller
        self.log.write(json.dumps({"time": started, "namespace": name, "inode": inode, "argv": list(arguments),
                                   "rc": result.returncode, "stdout": result.stdout, "stderr": result.stderr}) + "\n")
        if check and result.returncode:
            raise RuntimeError(f"{name}: {' '.join(arguments)}: {result.stderr.strip() or result.stdout.strip()}")
        return result

    def prefix(self, name):
        if os.readlink("/proc/self/ns/net") != self.controller:
            raise RuntimeError("Controller changed network namespace")
        if name is None:
            return []
        child, inode = self.namespaces[name]
        path = f"/proc/{child.pid}/ns/net"
        if child.poll() is not None or os.readlink(path) != inode or inode == self.host_namespace:
            raise RuntimeError(f"Namespace {name} is no longer owned by this run")
        return ["nsenter", f"--net={path}", "--"]

    def spawn(self, name, arguments, **kwargs):
        child = subprocess.Popen(self.prefix(name) + list(arguments), **kwargs)
        self.children.append(child)
        return child

    def create_namespace(self, name):
        keeper = "import os,sys; print(os.getpid(),flush=True); sys.stdin.buffer.read()"
        child = subprocess.Popen(["unshare", "--net", "--", "python3", "-u", "-c", keeper],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.children.append(child)
        if not select.select([child.stdout], [], [], 5)[0] or child.stdout.readline().strip() != str(child.pid):
            raise RuntimeError(f"Cannot create namespace {name}")
        inode = os.readlink(f"/proc/{child.pid}/ns/net")
        if inode in {self.controller, self.host_namespace}:
            raise RuntimeError(f"Namespace isolation failed for {name}")
        self.namespaces[name] = (child, inode)
        self.command(name, ["ip", "link", "set", "lo", "up"])
        self.command(name, ["sysctl", "-qw", "net.ipv6.conf.all.disable_ipv6=1"])
        self.command(name, ["sysctl", "-qw", "net.ipv4.conf.all.rp_filter=0"])
        self.command(name, ["sysctl", "-qw", "net.ipv4.conf.default.rp_filter=0"])

    def link(self, left, left_interface, right, right_interface):
        self.serial += 1
        a, b = f"v{self.serial}a", f"v{self.serial}b"
        self.command(None, ["ip", "link", "add", a, "type", "veth", "peer", "name", b])
        for temporary, name, interface in ((a, left, left_interface), (b, right, right_interface)):
            self.command(None, ["ip", "link", "set", temporary, "netns", str(self.namespaces[name][0].pid)])
            self.command(name, ["ip", "link", "set", temporary, "name", interface])
            self.command(name, ["ethtool", "-K", interface, "gro", "off", "gso", "off", "tso", "off"])
            self.command(name, ["ip", "link", "set", interface, "up"])

    def bridge(self, name, address):
        self.command(name, ["ip", "link", "add", "lan", "type", "bridge"])
        self.command(name, ["ip", "addr", "add", address + "/24", "dev", "lan"])
        self.command(name, ["ip", "link", "set", "lan", "up"])
        self.command(name, ["sysctl", "-qw", "net.ipv4.ip_forward=1"])

    def attach(self, node, gateway, port):
        self.link(node, "eth0" if node.startswith("n") else "wan", gateway, port)
        self.command(gateway, ["ip", "link", "set", port, "master", "lan"])

    def netem(self, node, source_country, *, rtt=None, loss=None):
        index = next(i + 1 for i, c in enumerate(COUNTRIES) if c.name == source_country)
        same = source_country == node.country
        link = None if same else wan(source_country, node.country)
        delay = (2 if same else link.rtt_ms) / 2 if rtt is None else rtt / 2
        jitter = 0.2 if same else link.jitter_ms
        loss = (0 if same else link.loss_percent) if loss is None else loss
        self.command(node.name, ["tc", "qdisc", "replace", "dev", "ifb0", "parent", f"1:{index}",
                                "handle", f"{index + 10}:", "netem", "limit", "4096", "delay", f"{delay}ms",
                                f"{jitter}ms", "distribution", "normal", "loss", "random", f"{loss}%",
                                "seed", str(self.seed + node.number * 10 + index)])

    def receiver(self, node):
        self.command(node.name, ["ip", "link", "add", "ifb0", "type", "ifb"])
        self.command(node.name, ["ip", "link", "set", "ifb0", "up"])
        self.command(node.name, ["tc", "qdisc", "add", "dev", "eth0", "clsact"])
        self.command(node.name, ["tc", "filter", "add", "dev", "eth0", "ingress", "matchall", "action",
                                "mirred", "egress", "redirect", "dev", "ifb0"])
        own = next(i + 1 for i, c in enumerate(COUNTRIES) if c.name == node.country)
        self.command(node.name, ["tc", "qdisc", "add", "dev", "ifb0", "root", "handle", "1:", "htb", "default", str(own)])
        for index, country in enumerate(COUNTRIES, 1):
            self.command(node.name, ["tc", "class", "add", "dev", "ifb0", "parent", "1:", "classid", f"1:{index}",
                                    "htb", "rate", "1gbit", "ceil", "1gbit"])
            self.netem(node, country.name)
            self.command(node.name, ["tc", "filter", "add", "dev", "ifb0", "parent", "1:", "protocol", "ip",
                                    "prio", str(index), "flower", "src_ip", country.subnet, "classid", f"1:{index}"])

    def bandwidth(self, link, mbps):
        for country in (link.a, link.b):
            self.command("r" + country, ["tc", "qdisc", "replace", "dev", "w_" + link.a + link.b,
                                         "root", "tbf", "rate", f"{mbps}mbit", "burst", "64kb", "latency", "100ms"])

    def build(self):
        for name in [n.name for n in NODES] + [c.router for c in COUNTRIES] + [g.name for g in NATS]:
            self.create_namespace(name)
        for country in COUNTRIES:
            self.bridge(country.router, country.gateway)
        for gateway in NATS:
            self.bridge(gateway.name, "10.64.1.1")
            self.attach(gateway.name, "r" + gateway.country, "p" + gateway.name)
            self.command(gateway.name, ["ip", "addr", "add", gateway.ip + "/24", "dev", "wan"])
            country = next(c for c in COUNTRIES if c.name == gateway.country)
            self.command(gateway.name, ["ip", "route", "add", "default", "via", country.gateway])
            rules = nat_rules(gateway)
            (self.directory / f"{gateway.name}.nft").write_text(rules)
            self.command(gateway.name, ["nft", "-f", "-"], input=rules)
        for node in NODES:
            self.attach(node.name, node.nat or "r" + node.country, "p" + node.name)
            self.command(node.name, ["ip", "addr", "add", node.ip + "/24", "dev", "eth0"])
            self.command(node.name, ["ip", "route", "add", "default", "via", node.gateway])
            self.receiver(node)
        for link in WANS:
            interface = "w_" + link.a + link.b
            self.link("r" + link.a, interface, "r" + link.b, interface)
            for country, address, peer, destination in ((link.a, link.ip_a, link.ip_b, link.b),
                                                       (link.b, link.ip_b, link.ip_a, link.a)):
                subnet = next(c.subnet for c in COUNTRIES if c.name == destination)
                self.command("r" + country, ["ip", "addr", "add", address + "/30", "dev", interface])
                self.command("r" + country, ["ip", "route", "add", subnet, "via", peer])
            self.bandwidth(link, link.mbps)
        state = {name: {"pid": child.pid, "namespace": inode} for name, (child, inode) in self.namespaces.items()}
        (self.directory / "namespaces.json").write_text(json.dumps(state, indent=2) + "\n")

    def snapshot(self, label):
        snapshot = {}
        for name in self.namespaces:
            snapshot[name] = {"links": json.loads(self.command(name, ["ip", "-j", "-s", "link"]).stdout),
                              "routes": json.loads(self.command(name, ["ip", "-j", "route"]).stdout),
                              "qdisc": json.loads(self.command(name, ["tc", "-j", "-s", "qdisc"]).stdout)}
            if name.startswith("g_"):
                snapshot[name]["nft"] = json.loads(self.command(name, ["nft", "-j", "list", "ruleset"]).stdout)
        (self.directory / f"network-{label}.json").write_text(json.dumps(snapshot, indent=2) + "\n")

    def close(self):
        for child in reversed(self.children):
            if child.poll() is None:
                child.terminate()
        for child in reversed(self.children):
            try:
                child.wait(timeout=3)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait(timeout=3)
        self.log.close()
