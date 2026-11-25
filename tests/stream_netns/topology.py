"""Описание изолированного стенда; здесь нет сетевых команд и привилегий."""

from dataclasses import asdict, dataclass


@dataclass(frozen=True)
class Country:
    name: str
    subnet: str
    router: str
    gateway: str


@dataclass(frozen=True)
class Node:
    number: int
    country: str
    ip: str
    gateway: str
    nat: str | None
    client_type: int
    udp_port: int = 32000

    @property
    def name(self):
        return f"n{self.number:02d}"

    @property
    def mapped_port(self):
        return 23000 + self.number


@dataclass(frozen=True)
class Nat:
    name: str
    country: str
    ip: str
    mode: str


@dataclass(frozen=True)
class Wan:
    a: str
    b: str
    ip_a: str
    ip_b: str
    rtt_ms: int
    jitter_ms: float
    loss_percent: float
    mbps: int


COUNTRIES = (
    Country("a", "192.0.2.0/24", "ra", "192.0.2.1"),
    Country("b", "198.51.100.0/24", "rb", "198.51.100.1"),
    Country("c", "203.0.113.0/24", "rc", "203.0.113.1"),
)
NATS = tuple(
    Nat(f"g_{c.name}{i}", c.name, c.gateway.rsplit(".", 1)[0] + f".{100 + i}", mode)
    for c in COUNTRIES
    for i, mode in ((1, "restricted" if c.name == "c" else "cone"), (2, "strict"))
)
WANS = (
    Wan("a", "b", "198.18.0.1", "198.18.0.2", 50, 2, 0.2, 50),
    Wan("a", "c", "198.18.0.5", "198.18.0.6", 100, 5, 0.5, 30),
    Wan("b", "c", "198.18.0.9", "198.18.0.10", 150, 10, 1.0, 20),
)


def nodes():
    result = []
    number = 1
    for country, private_count in zip(COUNTRIES, (4, 3, 3)):
        prefix = country.gateway.rsplit(".", 1)[0]
        for public in range(2):
            result.append(Node(number, country.name, f"{prefix}.{11 + public}", country.gateway, None, 0 if not public else 1))
            number += 1
        for private in range(private_count):
            nat = f"g_{country.name}{1 + private // 2}"
            result.append(Node(number, country.name, f"10.64.1.{2 + private % 2}", "10.64.1.1", nat, 2 if number == 16 else 1))
            number += 1
    return tuple(result)


NODES = nodes()


def wan(a, b):
    return next(link for link in WANS if {link.a, link.b} == {a, b})


def manifest(seed):
    return {"version": 1, "seed": seed, "countries": [asdict(c) for c in COUNTRIES],
            "nodes": [dict(asdict(n), name=n.name, mapped_port=n.mapped_port) for n in NODES],
            "nat": [asdict(n) for n in NATS], "wan": [asdict(w) for w in WANS],
            "clock": "CLOCK_MONOTONIC, no time namespace", "impairment": "receiver ingress IFB",
            "private_subnet": "10.64.1.0/24 (reused independently by every NAT)"}
