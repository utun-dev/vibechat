"""Управление настоящими процессами uTun; сетевые пакеты не подменяются."""

import csv
import json
from pathlib import Path
import select
import subprocess
import time

from topology import NODES, WANS


class Agent:
    def __init__(self, network, node, binary, seed, tcp, tcp_only, core_only=False, extra_config=""):
        self.node = node
        directory = network.directory / node.name
        directory.mkdir()
        (directory / "db").mkdir()
        identity = json.loads(subprocess.check_output([str(binary), "--identity", f"stream-lab:{seed}:{node.name}"], text=True))
        self.ed25519 = identity["ed25519"]
        kind = "nat" if node.nat else "public"
        sockets = "".join(f"[server:{transport}]\naddr={node.ip}:32000\ntype={kind}\ntransport={transport}\n"
                          for transport in (["tcp"] if tcp_only else ["udp", "tcp"] if tcp else ["udp"]))
        # В NAT-сценарии поздний запрос TCP должен открыть линк и при уже работающем UDP.
        policy = "connection_order=TUR3\n" if core_only and tcp else ""
        config = (f"[global]\nmy_node_name={node.name}\nmy_private_key={identity['private']}\n"
                  f"my_public_key={identity['public']}\nclient_type={node.client_type}\nauto_sockets=no\n"
                  f"db_path={directory / 'db'}\ndebug_level=warn\n{policy}"
                  "[ntp]\nenabled=no\n[allowed_keys]\nallow_all=1\n"
                  "[chatserver]\ngroup_autoconnect=1\nstorage_autoload=0\nmember_files_enabled=0\n"
                  "[debug]\nstream=debug\nvideo=info\nnat=debug\nchat_sync=info\nmember_sync=info\nbgp=debug\nconnection=info\n" + sockets + extra_config)
        path = directory / "node.conf"
        path.write_text(config)
        self.stderr = (directory / "stderr.log").open("w")
        self.transcript = (directory / "rpc.jsonl").open("w", buffering=1)
        command = [str(binary), str(path), str(directory / "utun.log")]
        if core_only:
            command.append("--core-only")
        self.process = network.spawn(node.name, command,
                                     stdin=-1, stdout=-1, stderr=self.stderr, text=True, bufsize=1)
        self.serial = 0
        if self.read(10) != {"ready": True, "node": self.ed25519}:
            raise RuntimeError(f"{node.name}: unexpected identity/ready response")

    def read(self, timeout):
        if not select.select([self.process.stdout], [], [], timeout)[0]:
            raise RuntimeError(f"{self.node.name}: RPC timed out ({timeout}s); see utun.log")
        line = self.process.stdout.readline()
        self.transcript.write(json.dumps({"direction": "out", "time": time.monotonic(), "line": line.rstrip()}) + "\n")
        if not line:
            raise RuntimeError(f"{self.node.name}: agent exited rc={self.process.poll()}; see stderr.log/utun.log")
        try:
            return json.loads(line)
        except json.JSONDecodeError as error:
            raise RuntimeError(f"{self.node.name}: non-JSON stdout: {line[:200]}") from error

    def rpc(self, op, *, timeout=5, allow_wait=False, **arguments):
        self.serial += 1
        request = {key: str(value) for key, value in dict(arguments, op=op, id=self.serial).items()}
        self.transcript.write(json.dumps({"direction": "in", "time": time.monotonic(), "request": request}) + "\n")
        self.process.stdin.write(json.dumps(request) + "\n")
        self.process.stdin.flush()
        response = self.read(timeout)
        if response.get("id") != self.serial or response.get("rc") not in ({0, 2} if allow_wait else {0}):
            raise RuntimeError(f"{self.node.name}: {op} failed: {response}")
        return response

    def close(self):
        if self.process.poll() is None:
            try:
                self.rpc("quit")
                self.process.wait(timeout=10)
            except Exception:
                self.process.kill()
                self.process.wait()
        self.stderr.close()
        self.transcript.close()
        return self.process.returncode


class Scenario:
    def __init__(self, network, args):
        self.network, self.args = network, args
        self.agents = {}
        self.samples = (network.directory / "samples.jsonl").open("w", buffering=1)
        self.started = time.monotonic()
        self.phases = []
        self.latest = {}
        self.last_metadata = {}
        self.clock_previous = {}
        self.clock_wraps = 0

    def sample(self, phase):
        snapshot = {name: agent.rpc("stats", loads=int(self.args.resources)) for name, agent in self.agents.items()}
        self.latest = snapshot
        self.samples.write(json.dumps({"elapsed_s": time.monotonic() - self.started, "phase": phase, "nodes": snapshot}) + "\n")
        for name, statistics in snapshot.items():
            if statistics["errors"] or any(s["errors"] for s in statistics["streams"]):
                raise RuntimeError(f"{name}: payload/order validation failed; see samples.jsonl and utun.log")
            if any(s.get("stored_bytes", 0) > 32 * 1024 * 1024 for s in statistics["streams"]):
                raise RuntimeError(f"{name}: stream data buffer exceeded 32 MiB")
            for stream in statistics["streams"]:
                self.last_metadata.setdefault((name, stream["source"]), {}).update(stream)
                path = next((p for p in stream.get("paths", []) if p["sub"] == 0), {})
                if stream["publisher"] and "source_timestamp_ms" in path:
                    key = (name, stream["source"])
                    current = (path["seq"], path["source_timestamp_ms"])
                    previous = self.clock_previous.get(key)
                    if previous and current[0] > previous[0] and current[1] < previous[1]:
                        self.clock_wraps += 1
                    self.clock_previous[key] = current
        return snapshot

    def wait(self, phase, predicate, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            statistics = self.sample(phase)
            if predicate(statistics):
                return statistics
            time.sleep(1)
        raise RuntimeError(f"{phase}: condition not met in {timeout}s; see last samples.jsonl record")

    def duration(self, phase, seconds, observe=None):
        print(f"Phase {phase}: {seconds}s", flush=True)
        started = time.monotonic()
        self.phases.append({"phase": phase, "begin_s": started - self.started, "duration_s": seconds})
        while time.monotonic() - started < seconds:
            snapshot = self.sample(phase)
            if observe:
                observe(snapshot)
            time.sleep(1)

    @staticmethod
    def stream(statistics, source):
        return next((s for s in statistics["streams"] if s["source"] == source), {})

    def ready(self, statistics, source, names):
        return all(self.stream(statistics[name], source).get("received", 0) >= 10 and
                   self.stream(statistics[name], source).get("age_ms", 100000) < 5000 for name in names)

    def subscribe_wave(self, source_id, names):
        pending = set(names)
        deadline = time.monotonic() + 90
        while pending and time.monotonic() < deadline:
            for name in sorted(pending):
                if self.agents[name].rpc("subscribe", source=source_id, allow_wait=True)["rc"] == 0:
                    pending.remove(name)
            if pending:
                time.sleep(1)
        if pending:
            raise RuntimeError(f"Signed source profile did not reach subscribers: {sorted(pending)}")

    def churn(self, source_id, names):
        for cycle in range(self.args.churn_cycles):
            # Выбираем по фактическим DATA-путям: выключение просто зрителя не проверило бы downstream recovery.
            uses = {name: 0 for name in names}
            identities = {agent.ed25519: name for name, agent in self.agents.items()}
            for name in names:
                for path in self.stream(self.latest[name], source_id).get("paths", []):
                    for identity in path["nodes"]:
                        relay = identities.get(identity)
                        if relay in uses and relay != name:
                            uses[relay] += 1
            relay = max(sorted(uses), key=uses.get)
            if not uses[relay]:
                raise RuntimeError("Churn needs a real intermediate relay; all observed paths are direct")
            print(f"Churn {cycle + 1}: disconnect {relay}, used by {uses[relay]} DATA paths", flush=True)
            before = {name: self.stream(self.latest[name], source_id)["received"] for name in names}
            self.network.command(relay, ["ip", "link", "set", "eth0", "down"])
            try:
                self.duration(f"relay-offline-{cycle + 1}", 35)
                others = names - {relay}
                self.wait(f"relay-bypass-{cycle + 1}", lambda s: self.ready(s, source_id, others) and
                          all(self.stream(s[n], source_id)["received"] > before[n] for n in others), 90)
            finally:
                self.network.command(relay, ["ip", "link", "set", "eth0", "up"])
                # Linux удаляет static default route при link down; стенд исполняет здесь роль network manager.
                gateway = next(node.gateway for node in NODES if node.name == relay)
                self.network.command(relay, ["ip", "route", "replace", "default", "via", gateway, "dev", "eth0"])
            before = {name: self.stream(self.latest[name], source_id)["received"] for name in names}
            self.wait(f"relay-return-{cycle + 1}", lambda s: self.ready(s, source_id, names) and
                      all(self.stream(s[n], source_id)["received"] > before[n] for n in names), 90)
            self.duration(f"relay-restored-{cycle + 1}", 10)

    def validate_end(self, final, source_id, publisher, names):
        generated = self.stream(final[publisher], source_id)["generated"]
        if generated < 10:
            raise RuntimeError(f"{publisher}: generated insufficient data")
        for name in names:
            stream = self.stream(final[name], source_id)
            if stream["next_seq"] != generated:
                raise RuntimeError(f"{name}/{publisher}: END delivered before final cursor: {stream['next_seq']} != {generated}")
            if stream["received"] + stream["gap_batches"] + stream["start_seq"] != generated:
                raise RuntimeError(f"{name}/{publisher}: unaccounted records: received={stream['received']}, "
                                   f"gaps={stream['gap_batches']}, start={stream['start_seq']}, generated={generated}")
        return generated

    def resources(self, source_id, names):
        publisher = self.args.publisher
        source = self.agents[publisher]
        before = {name: self.stream(self.latest[name], source_id)["received"] for name in names}
        source.rpc("cpu", seconds=10)
        try:
            busy = self.wait("cpu-admission", lambda s: s[publisher]["uasync_cpu_valid"] and
                             s[publisher]["uasync_cpu_pct"] > 80 and s[publisher]["peer_load"] and
                             all(p["global_pressure"] and not p["accepting"] for p in s[publisher]["peer_load"]), 8)
            cpu_peak = busy[publisher]["uasync_cpu_pct"]
            self.duration("cpu-loaded", 5)
            if any(self.stream(self.latest[n], source_id)["received"] <= before[n] for n in names):
                raise RuntimeError("CPU load: an existing viewer made no progress")
        finally:
            source.rpc("cpu", seconds=0)
        self.wait("cpu-recovery", lambda s: s[publisher]["uasync_cpu_valid"] and s[publisher]["uasync_cpu_pct"] <= 70
                  and s[publisher]["peer_load"] and not any(p["global_pressure"] for p in s[publisher]["peer_load"])
                  and self.ready(s, source_id, names), 60)
        baseline = {p["peer"]: p["overload_id"] for p in self.latest[publisher]["peer_load"]}
        evidence = {"cpu_admission_percent": cpu_peak, "uplink_bps": self.args.rate * 2,
                    "low_cpu_overload": False, "released_flows": False}

        def observe(snapshot):
            node = snapshot[publisher]
            for peer in node["peer_load"]:
                if peer["overload_id"] <= baseline.get(peer["peer"], 0):
                    continue
                if node["uasync_cpu_valid"] and node["uasync_cpu_pct"] <= 70 and not peer["global_pressure"]:
                    evidence["low_cpu_overload"] = True
                if peer["released_count"]:
                    evidence["released_flows"] = True

        self.network.command(publisher, ["tc", "qdisc", "replace", "dev", "eth0", "root", "tbf",
                                         "rate", f"{evidence['uplink_bps']}bit", "burst", "32kb", "latency", "100ms"])
        try:
            self.duration("uplink-limited", 30, observe)
            self.network.snapshot("uplink-limited")
        finally:
            self.network.command(publisher, ["tc", "qdisc", "del", "dev", "eth0", "root"])
        if not evidence["low_cpu_overload"] or not evidence["released_flows"]:
            raise RuntimeError(f"Shared uplink did not exercise low-CPU OVERLOAD and flow release: {evidence}")
        before = {name: self.stream(self.latest[name], source_id)["received"] for name in names}
        self.wait("uplink-recovery", lambda s: self.ready(s, source_id, names) and
                  all(self.stream(s[n], source_id)["received"] > before[n] for n in names), 90)
        self.duration("resource-restored", 10)
        return evidence

    def run(self):
        binary = Path(self.args.binary).resolve()
        if self.args.stun_only:
            from stun_scenario import check_stun
            return check_stun(self)
        if self.args.nat_single_peer:
            from nat_scenario import check_single_peer_profiles
            return check_single_peer_profiles(self, Agent, binary)
        nat_check = self.args.nat_check or self.args.nat_only
        for node in NODES:
            self.agents[node.name] = Agent(self.network, node, binary, self.args.seed, self.args.tcp, self.args.tcp_only, nat_check)
        nat_result = None
        if nat_check:
            from nat_scenario import check_profiles
            nat_result = check_profiles(self)
            if self.args.nat_only:
                return {"pass": True, "nat": nat_result, "transport": "tcp" if self.args.tcp_only else
                        "udp+tcp" if self.args.tcp else "udp"}
            for agent in self.agents.values():
                agent.rpc("nat_auto", enabled=1)
                agent.rpc("chat_start")
        owner = self.agents["n01"]
        group = owner.rpc("create")["group"]
        print(f"Joining real CHAT group {group}", flush=True)
        ordered = [n for n in NODES if not n.nat and n.name != "n01"] + [n for n in NODES if n.nat]
        link = owner.rpc("invite", timeout=15)["link"]
        for node in ordered:
            self.agents[node.name].rpc("join", link=link)
        identities = {agent.ed25519 for agent in self.agents.values()}
        self.wait("join-merkle-bgp", lambda s: all(set(n["online"]) == identities and
                  set(n["profile_status"]) == identities and all(rc == 0 for rc in n["profile_status"].values())
                  for n in s.values()), 120)
        if nat_check:
            for agent in self.agents.values():
                agent.rpc("nat_release")
        source = self.agents[self.args.publisher]
        source_id = source.ed25519
        source.rpc("publish", seed=self.args.seed, n=self.args.n, r=self.args.r)
        names = set(self.agents) - {self.args.publisher}
        ordered_names = sorted(names)
        # При одном массовом приходе сохраняем исходный baseline: публикация начинается после подписки всех.
        if self.args.join_wave_size < len(names):
            source.rpc("rate", source=source_id, bps=self.args.rate)
        for start in range(0, len(names), self.args.join_wave_size):
            self.subscribe_wave(source_id, ordered_names[start:start + self.args.join_wave_size])
            if start + self.args.join_wave_size < len(names):
                self.duration(f"join-wave-{start // self.args.join_wave_size + 1}", self.args.join_wave_interval)
        source.rpc("rate", source=source_id, bps=self.args.rate)
        self.wait("initial-delivery", lambda s: self.ready(s, source_id, names), 90)
        second = None
        if self.args.second_publisher:
            second = self.agents[self.args.second_publisher]
            second.rpc("publish", seed=self.args.seed, n=self.args.n, r=self.args.r)
            second_names = set(self.agents) - {self.args.second_publisher}
            self.subscribe_wave(second.ed25519, second_names)
            second.rpc("rate", source=second.ed25519, bps=self.args.rate)
            self.wait("second-stream-delivery", lambda s: self.ready(s, second.ed25519, second_names), 90)
        self.duration("baseline", self.args.duration)
        resources = self.resources(source_id, names) if self.args.resources else None
        self.churn(source_id, names)
        if self.args.faults:
            link = WANS[1]
            self.network.bandwidth(link, 2)
            for node in NODES:
                if node.country in {link.a, link.b}:
                    other = link.b if node.country == link.a else link.a
                    self.network.netem(node, other, rtt=300, loss=5)
            self.duration("wan-degraded", self.args.fault_duration)
            before_recovery = {name: self.stream(self.latest[name], source_id)["received"] for name in names}
            self.network.bandwidth(link, link.mbps)
            for node in NODES:
                if node.country in {link.a, link.b}:
                    self.network.netem(node, link.b if node.country == link.a else link.a)
            self.wait("wan-recovery", lambda s: self.ready(s, source_id, names) and
                      all(self.stream(s[name], source_id)["received"] > before_recovery[name] for name in names), 90)
            self.duration("recovered", 10)
            # Узлы без подписки должны прекратить обработку этого экземпляра.
            excluded = [name for name in ("n02", "n13") if name != self.args.publisher]
            for name in excluded:
                self.agents[name].rpc("close", source=source_id)
            counts = {name: self.stream(self.agents[name].rpc("stats"), source_id)["received"] for name in excluded}
            self.duration("unsubscribed", 5)
            for name in excluded:
                if self.stream(self.latest[name], source_id)["received"] != counts[name]:
                    raise RuntimeError(f"{name}: received DATA after subscription close")
            names -= set(excluded)
        source.rpc("finish", source=source_id)
        if second:
            second.rpc("finish", source=second.ed25519)
        final = self.wait("signed-end", lambda s: all(self.stream(s[n], source_id).get("ends", 0) for n in names) and
                          (not second or all(self.stream(s[n], second.ed25519).get("ends", 0) for n in second_names)), 60)
        generated = self.validate_end(final, source_id, self.args.publisher, names)
        if self.args.duration >= 66 and not self.clock_wraps:
            raise RuntimeError("Full-duration run did not observe a source timestamp wrap")
        self.report(final, source_id)
        second_result = None
        if second:
            second_generated = self.validate_end(final, second.ed25519, self.args.second_publisher, second_names)
            self.report(final, second.ed25519, "-second")
            second_result = {"publisher": self.args.second_publisher, "source": second.ed25519,
                             "generated": second_generated, "subscribers_checked": len(second_names)}
        return {"pass": True, "group": group, "publisher": self.args.publisher, "generated": generated,
                "phases": self.phases, "subscribers_checked": len(names), "source_timestamp_wraps": self.clock_wraps,
                "transport": "tcp" if self.args.tcp_only else "udp+tcp" if self.args.tcp else "udp",
                "rate_bps": self.args.rate, "n": self.args.n, "r": self.args.r, "nat": nat_result,
                "join_wave_size": self.args.join_wave_size, "churn_cycles": self.args.churn_cycles,
                "second_stream": second_result, "resources": resources}

    def report(self, final, source, label=""):
        rows = []
        for node in NODES:
            stream = self.stream(final[node.name], source)
            metadata = self.last_metadata[(node.name, source)]
            rows.append(dict(node=node.name, country=node.country, nat=node.nat or "public",
                             received=stream["received"], gap_batches=stream["gap_batches"], errors=stream["errors"],
                             p50_ms=stream["latency_p50_ms"], p95_ms=stream["latency_p95_ms"],
                             p99_ms=stream["latency_p99_ms"], maximum_ms=stream["latency_max_ms"],
                             quantile_limit_ms=stream["latency_limit_ms"],
                             migrations=metadata.get("migrations", 0), probes=metadata.get("probes", 0)))
        with (self.network.directory / f"summary{label}.csv").open("w", newline="") as output:
            writer = csv.DictWriter(output, fieldnames=list(rows[0]))
            writer.writeheader(); writer.writerows(rows)
        if not label:
            (self.network.directory / "final-stats.json").write_text(json.dumps(final, indent=2) + "\n")

    def close(self):
        (self.network.directory / "last-stats.json").write_text(json.dumps(self.latest, indent=2) + "\n")
        failures = {}
        for name, agent in self.agents.items():
            status = agent.close()
            if status:
                failures[name] = status
        self.samples.close()
        return failures
