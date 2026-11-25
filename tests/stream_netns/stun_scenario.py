"""STUN даёт предварительный EIM? того же IP/netif; ETCP уточняет профиль рабочего порта."""

import json
from pathlib import Path
import select
import sys

from nat_scenario import profile
from network import nat_rules
from scenario import Agent
from topology import NATS, NODES


def check_stun(scenario):
    network, agents = scenario.network, scenario.agents
    nodes = {node.name: node for node in NODES}
    script = Path(__file__).with_name("stun_server.py")
    servers = []
    outputs = []
    verified = {}
    try:
        for name, peer in (("n01", "n02"), ("n02", "n01")):
            output = (network.directory / f"{name}-stun.log").open("w", buffering=1)
            outputs.append(output)
            process = network.spawn(name, [sys.executable, "-u", str(script), "--ip", nodes[name].ip,
                                           "--peer", nodes[peer].ip], stdout=-1, stderr=output, text=True, bufsize=1)
            if not select.select([process.stdout], [], [], 5)[0] or process.stdout.readline().strip() != "READY":
                raise RuntimeError(f"STUN server {name} did not start")
            # Журнал читается процессом-копировщиком, чтобы не забить stdout pipe.
            network.spawn(name, [sys.executable, "-u", "-c", "import sys; sys.stdout.write(sys.stdin.read())"],
                          stdin=process.stdout, stdout=output)
            servers.append(process)
        cases = (("n03", "n07", "n12", "EIM", "OPEN"),
                 ("n14", "n08", "n13", "EIM", "CONTACT_REQUIRED"),
                 ("n05", "n01", "n02", "DEPENDENT", "CONTACT_REQUIRED"))
        for owner_name, observer_name, helper_name, mapping, inbound in cases:
            owner = nodes[owner_name]
            gateway = next(item for item in NATS if item.name == owner.nat)
            network.command(gateway.name, ["nft", "flush", "ruleset"])
            network.command(gateway.name, ["nft", "-f", "-"], input=nat_rules(gateway, stun_trial=True))
            config = (f"[stun]\nenabled=yes\nserver={nodes['n01'].ip}:3478\nserver={nodes['n02'].ip}:3478\n"
                      f"[server:duplicate]\naddr={owner.ip}:32001\ntype=nat\n")
            agents[owner_name] = Agent(network, owner, Path(scenario.args.binary).resolve(), scenario.args.seed,
                                       False, False, True, config)
            agents[owner_name].rpc("nat_auto", enabled=1)
            data = scenario.wait(f"stun-diagnostics-{owner_name}", lambda statistics:
                                 all(item.get("stun", {}).get("status") in (1, 2)
                                     for item in statistics[owner_name]["address_profiles"]), 130)
            udp, duplicate = profile(data[owner_name], "udp"), profile(data[owner_name], "duplicate")
            expected_mapping = {"DEPENDENT", "ADM", "APDM"} if mapping == "DEPENDENT" else {mapping}
            expected_filtering = {"EIF"} if inbound == "OPEN" else {"ADF", "APDF"}
            if udp["stun"]["mapping"] not in expected_mapping or udp["stun"]["filtering"] not in expected_filtering:
                raise RuntimeError(f"{owner_name}: STUN diagnostic class mismatch: {udp}")
            estimated = mapping == "EIM"
            expected_source, expected_map = ("STUN", "EIM") if estimated else ("NONE", "UNKNOWN")
            data = scenario.wait(f"stun-tentative-{owner_name}", lambda statistics:
                                 all((profile(statistics[owner_name], name)["source"],
                                      profile(statistics[owner_name], name)["mapping"],
                                      profile(statistics[owner_name], name)["inbound"]) ==
                                     (expected_source, expected_map, "UNKNOWN") for name in ("udp", "duplicate")), 5)
            udp, duplicate = profile(data[owner_name], "udp"), profile(data[owner_name], "duplicate")
            if udp["stun"]["evidence"] != duplicate["stun"]["evidence"] or udp["stun"]["members"] != 2 or udp["external"]:
                raise RuntimeError(f"{owner_name}: duplicated probe or foreign external port: {udp}, {duplicate}")
            for name in (observer_name,):
                agents[name] = Agent(network, nodes[name], Path(scenario.args.binary).resolve(), scenario.args.seed,
                                      False, False, True)
            agents[owner_name].rpc("nat_link", snapshot=agents[observer_name].rpc("snapshot")["snapshot"], socket="udp")
            scenario.wait(f"stun-observer-link-{owner_name}", lambda statistics:
                          agents[observer_name].ed25519 in statistics[owner_name]["group_ready"] and
                          agents[owner_name].ed25519 in statistics[observer_name]["group_ready"], 20)
            agents[owner_name].rpc("nat_check", socket="udp", observer=agents[observer_name].ed25519)
            data = scenario.wait(f"stun-no-helper-{owner_name}", lambda statistics:
                                 profile(statistics[owner_name], "udp")["reason"] == "no_helper", 40)
            partial = profile(data[owner_name], "udp")
            expected_source = "STUN" if estimated else "ETCP"
            if (partial["source"], partial["mapping"], partial["inbound"]) != (expected_source, expected_map, "UNKNOWN"):
                raise RuntimeError(f"{owner_name}: single-observer profile mismatch: {partial}")
            if not partial["external"]:
                raise RuntimeError(f"{owner_name}: single observer did not preserve working external address")
            agents[helper_name] = Agent(network, nodes[helper_name], Path(scenario.args.binary).resolve(), scenario.args.seed,
                                        False, False, True)
            agents[observer_name].rpc("nat_link", snapshot=agents[helper_name].rpc("snapshot")["snapshot"], socket="udp")
            scenario.wait(f"stun-helper-link-{owner_name}", lambda statistics:
                          agents[helper_name].ed25519 in statistics[observer_name]["group_ready"] and
                          agents[observer_name].ed25519 in statistics[helper_name]["group_ready"], 20)
            agents[owner_name].rpc("nat_check", socket="udp", observer=agents[observer_name].ed25519)
            expected = ("DEPENDENT", "UNKNOWN") if gateway.mode == "strict" else (mapping, inbound)
            data = scenario.wait(f"stun-etcp-promotion-{owner_name}", lambda statistics:
                                 (profile(statistics[owner_name], "udp")["source"],
                                  profile(statistics[owner_name], "udp")["mapping"],
                                  profile(statistics[owner_name], "udp")["inbound"]) == ("ETCP", *expected), 40)
            measured = profile(data[owner_name], "udp")
            if (measured["mapping"], measured["inbound"]) != expected:
                raise RuntimeError(f"{owner_name}: ETCP class mismatch: {measured}")
            verified[owner_name] = {"stun": udp, "etcp": measured}
            print(f"PASS {owner_name}: STUN {mapping}/{inbound} -> ETCP {expected[0]}/{expected[1]}", flush=True)
            # Каждый следующий случай начинает с действительно одного наблюдателя.
            for name in (owner_name, observer_name, helper_name):
                if agents.pop(name).close():
                    raise RuntimeError(f"{name}: nonzero exit")
        (network.directory / "stun-result.json").write_text(json.dumps(verified, indent=2) + "\n")
        return {"pass": True, "stun": verified}
    finally:
        for process in servers:
            process.terminate()
            process.wait(timeout=5)
        for output in outputs:
            output.close()
