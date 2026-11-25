"""Проверка детектора uTun на настоящем NAT; контроль не создаёт контакт с помощником."""

import json

from topology import NATS, NODES
from network import nat_rules


def profile(statistics, protocol):
    return next(item for item in statistics["address_profiles"] if item["socket"] == protocol)


def check_single_peer_profiles(scenario, create_agent, binary):
    cases = (("a", "cone", ("EIM", "OPEN")), ("b", "strict", ("DEPENDENT", "UNKNOWN")),
             ("c", "restricted", ("EIM", "CONTACT_REQUIRED")))
    verified = {}
    for country, mode, expected in cases:
        gateway = next(g for g in NATS if (g.country, g.mode) == (country, mode))
        owner = next(node for node in NODES if node.nat == gateway.name)
        peer = next(node for node in NODES if node.country == country and not node.nat)
        alternate = peer.ip.rsplit(".", 1)[0] + ".20"
        scenario.network.command(peer.name, ["ip", "addr", "add", f"{alternate}/24", "dev", "eth0"])
        if mode == "strict":
            rules = nat_rules(gateway, extra_destinations=(alternate,))
            (scenario.network.directory / f"{gateway.name}.nft").write_text(rules)
            scenario.network.command(gateway.name, ["nft", "-f", "-"], input="delete table ip lab\n" + rules)
        extra = f"[server:alternate]\naddr={alternate}:32000\ntype=public\ntransport=udp\n"
        peer_agent = scenario.agents[peer.name] = create_agent(
            scenario.network, peer, binary, scenario.args.seed, scenario.args.tcp, False, core_only=True, extra_config=extra)
        owner_agent = scenario.agents[owner.name] = create_agent(
            scenario.network, owner, binary, scenario.args.seed, scenario.args.tcp, False, core_only=True)
        owner_agent.rpc("nat_auto", enabled=1)
        owner_agent.rpc("nat_link", snapshot=peer_agent.rpc("snapshot")["snapshot"], socket="udp")
        scenario.wait(f"single-nat-link-{owner.name}", lambda data:
                      data[owner.name]["group_ready"] == [peer_agent.ed25519] and
                      data[peer.name]["group_ready"] == [owner_agent.ed25519], 20)

        def finished(data):
            result = profile(data[owner.name], "udp")
            return result["rounds"] and result["phase"] == 0 and not result["cycling"]

        data = scenario.wait(f"single-nat-profile-{owner.name}", finished, 50)
        actual = profile(data[owner.name], "udp")
        external = "" if mode == "strict" else f"{gateway.ip}:{owner.mapped_port}"
        if ((actual["mapping"], actual["inbound"]) != expected or actual["external"] != external or
                actual["source"] != "ETCP" or actual["observer"] != peer_agent.ed25519 or
                actual["helper"] != peer_agent.ed25519 or actual["rounds"] != 1):
            raise RuntimeError(f"{owner.name}: expected one round through {peer.ip}/{alternate} yielding {expected}, got {actual}")
        verified[mode] = actual
        print(f"NAT single peer {peer.ip}/{alternate}, {owner.name}: {actual['mapping']} / {actual['inbound']}", flush=True)
    return {"pass": True, "nat_single_peer": verified}


def check_profiles(scenario):
    agents = scenario.agents
    public = [node for node in NODES if not node.nat]
    transport = "tcp" if scenario.args.tcp_only else "udp"
    connected = set()
    # Первый профиль измеряется при появлении маршрутов, до фонового обхода всех NODEINFO.
    for agent in agents.values():
        agent.rpc("nat_auto", enabled=1)

    def connect(source, target, protocol=transport):
        pair = (source.name, target.name, protocol)
        if pair in connected:
            return
        snapshot = agents[target.name].rpc("snapshot")["snapshot"]
        agents[source.name].rpc("nat_link", snapshot=snapshot, socket=protocol)
        target_id = agents[target.name].ed25519
        source_id = agents[source.name].ed25519
        scenario.wait(f"nat-link-{source.name}-{target.name}",
                      lambda data: target_id in data[source.name]["group_ready"] and
                      source_id in data[target.name]["group_ready"], 20)
        connected.add(pair)

    def store_owner(owner, observer):
        agents[observer.name].rpc("nat_store", snapshot=agents[owner.name].rpc("snapshot")["snapshot"])

    def measure(owner, observer, protocol):
        before = profile(agents[owner.name].rpc("stats"), protocol)
        agents[owner.name].rpc("nat_check", socket=protocol, observer=agents[observer.name].ed25519)
        data = scenario.wait(f"nat-profile-{owner.name}-{protocol}", lambda statistics:
                            profile(statistics[owner.name], protocol)["rounds"] > before["rounds"] and
                            profile(statistics[owner.name], protocol)["phase"] == 0 and
                            not profile(statistics[owner.name], protocol)["cycling"], 40)
        return profile(data[owner.name], protocol)

    # Отсутствие второго наблюдателя не превращается в STRICT/DIRECT.
    first, second = public[:2]
    connect(first, second)
    accepted = profile(agents[second.name].rpc("stats"), transport)
    if (accepted["mapping"], accepted["inbound"], accepted["source"]) != ("REAL_IP", "OPEN", "ETCP"):
        raise RuntimeError(f"NAT accepted public {transport} connection did not confirm DIRECT/OPEN: {accepted}")
    print(f"NAT accepted public {transport}: {second.name} DIRECT/OPEN without helper", flush=True)
    if not scenario.args.tcp_only:
        incomplete = measure(first, second, "udp")
        if (incomplete["mapping"], incomplete["inbound"], incomplete["reason"]) != ("UNKNOWN", "UNKNOWN", "no_helper"):
            raise RuntimeError(f"NAT no-helper test: {incomplete}")

        # Третий узел за NAT появляется через B: A не имеет ни линка, ни прежнего UDP-контакта с C.
        private_helper = next(node for node in NODES if node.nat)
        agents[first.name].rpc("nat_auto", enabled=1)
        connect(private_helper, second)
        data = scenario.wait("nat-routed-private-helper", lambda statistics:
                            (profile(statistics[first.name], "udp")["mapping"],
                             profile(statistics[first.name], "udp")["inbound"]) == ("REAL_IP", "OPEN"), 40)
        actual = profile(data[first.name], "udp")
        if actual["helper"] != agents[private_helper.name].ed25519 or agents[private_helper.name].ed25519 in data[first.name]["direct_up"]:
            raise RuntimeError(f"NAT indirect private helper was not used: {actual}")
        print(f"NAT routed private helper: {private_helper.name} through {second.name}, group={actual['group']}", flush=True)

    verified = {}
    protocols = ["tcp"] if scenario.args.tcp_only else ["udp", "tcp"] if scenario.args.tcp else ["udp"]
    for owner in public + [node for node in NODES if node.nat]:
        verified[owner.name] = {}
        if scenario.args.tcp_only:
            continue
        observer = second if owner.name == first.name else first
        if not scenario.args.tcp_only:
            agents[owner.name].rpc("nat_check", socket="udp", observer=agents[observer.name].ed25519)
        if not scenario.args.tcp_only:
            contacts = set(profile(agents[owner.name].rpc("stats"), "udp")["contacts"])
            helper = next((node for node in public if node.name not in {owner.name, observer.name} and node.ip not in contacts), None)
            initial = profile(agents[owner.name].rpc("stats"), "udp")
            if not helper and initial["source"] == "ETCP" and initial["mapping"] != "UNKNOWN":
                # Автоматическая детекция могла уже завершиться, пока узел помогал предыдущему owner.
                helper = next(node for node in public if node.name not in {owner.name, observer.name})
            if not helper:
                raise RuntimeError(f"{owner.name}: no independent helper remains before cold measurement; contacts={contacts}")
            connect(observer, helper)
        connect(owner, observer)
        store_owner(owner, observer)
        actual = measure(owner, observer, "udp")
        if not owner.nat:
            expected = ("REAL_IP", "OPEN")
        else:
            gateway = next(item for item in NATS if item.name == owner.nat)
            expected = {"cone": ("EIM", "OPEN"), "restricted": ("EIM", "CONTACT_REQUIRED"),
                        "strict": ("DEPENDENT", "UNKNOWN")}[gateway.mode]
        if (actual["mapping"], actual["inbound"]) != expected:
            raise RuntimeError(f"{owner.name}/udp: expected {expected}, detector returned {actual}")
        if expected[0] == "EIM":
            gateway = next(item for item in NATS if item.name == owner.nat)
            external = f"{gateway.ip}:{owner.mapped_port}"
        elif expected[0] == "REAL_IP":
            external = f"{owner.ip}:32000"
        else:
            external = ""
        if actual["external"] != external:
            raise RuntimeError(f"{owner.name}/udp: foreign/invalid external endpoint: {actual}, expected {external}")
        verified[owner.name]["udp"] = actual
        print(f"NAT {owner.name}/udp: {actual['mapping']} / {actual['inbound']}"
              f" external={actual['external'] or '-'} reason={actual['reason']}", flush=True)
        (scenario.network.directory / "nat-profiles.json").write_text(json.dumps(verified, indent=2) + "\n")

    # Дополнительные TCP-линки создаются после холодных UDP-измерений: NCD может запустить и UDP-попытки.
    if "tcp" in protocols:
        for owner in public + [node for node in NODES if node.nat]:
            observer = second if owner.name == first.name else first
            connect(owner, observer, "tcp")
            if not owner.nat:
                connect(observer, owner, "tcp")
            data = scenario.wait(f"nat-tcp-ip-{owner.name}", lambda statistics:
                                profile(statistics[owner.name], "tcp")["source"] == "ETCP" and
                                bool(profile(statistics[owner.name], "tcp")["external"]) and
                                (bool(owner.nat) or profile(statistics[owner.name], "tcp")["inbound"] == "OPEN"), 10)
            actual = profile(data[owner.name], "tcp")
            # Публичные listener принимают соединения в обе стороны; клиент за NAT даёт только IP:0.
            expected = ("UNKNOWN", "UNKNOWN") if owner.nat else ("REAL_IP", "OPEN")
            ip = next(item for item in NATS if item.name == owner.nat).ip if owner.nat else owner.ip
            if (actual["mapping"], actual["inbound"], actual["external"]) != (*expected, f"{ip}:0"):
                raise RuntimeError(f"{owner.name}/tcp: expected {expected}, IP={ip}:0, detector returned {actual}")
            if actual["rounds"] or actual["helper"] != "0" * 64:
                raise RuntimeError(f"{owner.name}: TCP IP detection unexpectedly used a helper round: {actual}")
            verified[owner.name]["tcp"] = actual
            print(f"NAT {owner.name}/tcp: {actual['mapping']} / {actual['inbound']} external={actual['external']}", flush=True)
        (scenario.network.directory / "nat-profiles.json").write_text(json.dumps(verified, indent=2) + "\n")

    # Повтор на использовавшемся порту сохраняет mapping, но не обновляет OPEN
    # по контактировавшим IP. Проверка запускается после всех холодных измерений.
    first_profile = verified[first.name].get("udp")
    if first_profile:
        again = measure(first, second, "udp")
        if again["mapping"] != "REAL_IP" or again["inbound"] not in {"OPEN", "UNKNOWN"}:
            raise RuntimeError(f"{first.name}: warm measurement produced invalid classification: {again}")
        if again["inbound"] == "UNKNOWN" and again["reason"] != "contact_history":
            raise RuntimeError(f"{first.name}: UNKNOWN without contact-history explanation: {again}")

    result = {"nodes_checked": len(verified), "protocols": protocols, "accepted_connection_checked": True,
              "no_helper_checked": not scenario.args.tcp_only,
              "warm_recheck_checked": bool(first_profile), "verified": verified}
    (scenario.network.directory / "nat-result.json").write_text(json.dumps(result, indent=2) + "\n")
    return result
