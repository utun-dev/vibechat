#!/usr/bin/env python3
"""Генератор воспроизводимых тестовых идентичностей с коллизиями BGP short31.

Требуется Python с cryptography. Каждая строка stdout — независимая пара в JSON.
Seed — little-endian номер кандидата; ключи предназначены для тестовых узлов.
"""
import argparse
import hashlib
import json

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat


def public_bytes(private):
    return private.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)


def identity(counter):
    seed = counter.to_bytes(32, "little")
    ed = public_bytes(Ed25519PrivateKey.from_private_bytes(seed))
    scalar = hashlib.sha512(seed).digest()[:32]
    x = public_bytes(X25519PrivateKey.from_private_bytes(scalar))
    return {"seed": seed.hex(), "ed25519": ed.hex(), "x25519": x.hex()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pairs", type=int, default=16, help="число разных коллизий")
    args = parser.parse_args()
    if not 1 <= args.pairs <= 2**31:
        parser.error("--pairs должен быть в диапазоне 1..2^31")

    seen = {}
    emitted = set()
    counter = 0
    while len(emitted) < args.pairs:
        seed = counter.to_bytes(32, "little")
        ed = public_bytes(Ed25519PrivateKey.from_private_bytes(seed))
        if not ed[31] & 0x80:  # Канонический знак устройства, как в sc_identity_from_seed().
            short = int.from_bytes(hashlib.sha256(ed).digest()[:4], "big") & 0x7fffffff
            if short not in emitted:
                first = seen.setdefault(short, counter)
                if first != counter:
                    pair = {"short31": short, "keys": [identity(first), identity(counter)]}
                    print(json.dumps(pair), flush=True)
                    emitted.add(short)
        counter += 1


if __name__ == "__main__":
    main()
