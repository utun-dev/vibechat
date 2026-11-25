#!/usr/bin/env python3
"""Запуск стенда: главный namespace используется только для чтения снимка."""

import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

from network import Network
from probe import Probes
from scenario import Scenario
from topology import manifest


def host_snapshot():
    result = {}
    for item, arguments in (("addresses", ["ip", "-j", "addr"]), ("routes", ["ip", "-j", "route", "show", "table", "all"]),
                             ("rules", ["ip", "-j", "rule"]), ("qdisc", ["tc", "-j", "qdisc"])):
        result[item] = json.loads(subprocess.check_output(arguments, text=True))
    return result


def stable_snapshot(value):
    # DHCP lifetime и route cache age меняются от часов, без изменения конфигурации.
    if isinstance(value, dict):
        return {key: stable_snapshot(item) for key, item in value.items()
                if key not in {"valid_life_time", "preferred_life_time", "expires", "used", "lastuse"}}
    if isinstance(value, list):
        return [stable_snapshot(item) for item in value]
    return value


def inside(args):
    def interrupted(signum, _frame):
        raise KeyboardInterrupt(f"signal {signum}")

    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    directory = Path(args.output)
    network = Network(directory, args.seed, args.host_netns)
    result = {"pass": False}
    probes = Probes(network)
    scenario = None
    try:
        print("Creating 25 isolated network namespaces (16 nodes, 3 routers, 6 NAT gateways)", flush=True)
        network.build()
        network.snapshot("calibration-initial")
        print("Calibrating WAN RTT and all 10 NAT clients", flush=True)
        probes.start()
        calibration = probes.calibrate()
        (directory / "calibration.json").write_text(json.dumps(calibration, indent=2) + "\n")
        probes.close()
        print("Network calibration passed", flush=True)
        if args.network_only:
            result["pass"] = True
        else:
            # Пробы не оставляют открытых mappings/conntrack для настоящих процессов uTun.
            network.snapshot("calibrated")
            network.close()
            print("Recreating network with empty NAT mappings and conntrack", flush=True)
            network = Network(directory, args.seed, args.host_netns)
            network.build()
            network.snapshot("initial")
            scenario = Scenario(network, args)
            result = scenario.run()
    except BaseException as error:
        result["error"] = str(error)
        print(f"FAIL: {type(error).__name__}: {error}", file=sys.stderr, flush=True)
    finally:
        probes.close()
        if scenario:
            failures = scenario.close()
            if failures:
                result["pass"] = False
                result["agent_exit_errors"] = failures
        try:
            network.snapshot("final")
        except Exception as error:
            result["snapshot_error"] = str(error)
        network.close()
        (directory / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return 0 if result["pass"] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", help="New directory for logs and results")
    parser.add_argument("--seed", type=int, default=20261011)
    parser.add_argument("--network-only", action="store_true", help="Only topology, WAN and NAT calibration")
    parser.add_argument("--nat-check", action="store_true", help="Verify owned endpoint profiles before real JOIN/streaming")
    parser.add_argument("--nat-only", action="store_true", help="Only real uTun endpoint detection after network calibration")
    parser.add_argument("--nat-single-peer", action="store_true", help="Detect real NAT with one ETCP peer on two public IPs")
    parser.add_argument("--stun-only", action="store_true", help="STUN interface fallback and promotion to ETCP on three real NAT classes")
    parser.add_argument("--binary", default=str(Path(__file__).parent / "stream_lab_node"))
    parser.add_argument("--publisher", choices=[f"n{i:02d}" for i in range(1, 17)], default="n01")
    parser.add_argument("--second-publisher", choices=[f"n{i:02d}" for i in range(1, 17)],
                        help="Run a second simultaneous stream at the same useful bitrate")
    parser.add_argument("--rate", type=int, default=1000000, help="Application bitrate in bits/s, before FEC")
    parser.add_argument("--n", type=int, default=8, help="Total RS parts (1..255)")
    parser.add_argument("--r", type=int, default=0, help="Redundant RS parts (0..n-1)")
    parser.add_argument("--duration", type=int, default=120, help="Baseline duration in real seconds")
    parser.add_argument("--faults", action="store_true", help="Degrade WAN, recover and check unsubscribe")
    parser.add_argument("--fault-duration", type=int, default=30)
    parser.add_argument("--join-wave-size", type=int, default=15, help="Subscribe viewers in waves (1..15)")
    parser.add_argument("--join-wave-interval", type=int, default=5, help="Seconds between subscription waves")
    parser.add_argument("--churn-cycles", type=int, default=0, help="Disconnect a busy relay for 35s, then restore it (0..10 cycles)")
    parser.add_argument("--resources", action="store_true", help="Exercise actual uasync CPU work and a shared publisher uplink shaper")
    transport = parser.add_mutually_exclusive_group()
    transport.add_argument("--tcp", action="store_true", help="Also expose TCP sockets, permitting ETCP multilink")
    transport.add_argument("--tcp-only", action="store_true", help="Expose only TCP, requiring the real STCP transport")
    parser.add_argument("--inside", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--host-netns", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.nat_single_peer and (args.tcp_only or args.stun_only or args.nat_only or args.nat_check or args.network_only):
        parser.error("--nat-single-peer runs a standalone UDP NAT test")
    if not 1 <= args.n <= 255 or not 0 <= args.r < args.n:
        parser.error("FEC requires 1 <= n <= 255 and 0 <= r < n")
    if args.second_publisher == args.publisher:
        parser.error("The second publisher must be another node")
    if not 1 <= args.join_wave_size <= 15 or args.join_wave_interval < 1 or not 0 <= args.churn_cycles <= 10:
        parser.error("Waves require 1..15 viewers and a positive interval; churn requires 0..10 cycles")
    if not args.network_only and (not Path(args.binary).is_file() or args.rate <= 0 or args.duration < 1 or args.fault_duration < 1):
        parser.error("Build stream_lab_node first; rate and durations must be positive")
    if args.inside:
        return inside(args)
    for program in ("ip", "tc", "nft", "ethtool", "sysctl", "unshare", "nsenter"):
        if not shutil.which(program):
            parser.error(f"Required tool is missing: {program}")
    directory = Path(args.output or Path(__file__).parent / "runs" / time.strftime("%Y%m%d-%H%M%S")).resolve()
    directory.mkdir(parents=True, exist_ok=False)
    (directory / "topology.json").write_text(json.dumps(manifest(args.seed), indent=2) + "\n")
    original_namespace = os.readlink("/proc/self/ns/net")
    before = host_snapshot()
    (directory / "host-before.json").write_text(json.dumps(before, indent=2) + "\n")
    command = ["unshare", "--user", "--map-root-user", "--keep-caps", "--mount", "--net", "--pid", "--fork",
               "--mount-proc", "--propagation", "private", "--kill-child", "--", sys.executable,
               str(Path(__file__).resolve()), "--inside", "--host-netns", original_namespace,
               "--output", str(directory), "--seed", str(args.seed), "--binary", str(Path(args.binary).resolve()),
               "--publisher", args.publisher, "--rate", str(args.rate), "--n", str(args.n), "--r", str(args.r), "--duration", str(args.duration),
               "--fault-duration", str(args.fault_duration), "--join-wave-size", str(args.join_wave_size),
               "--join-wave-interval", str(args.join_wave_interval), "--churn-cycles", str(args.churn_cycles)]
    if args.network_only:
        command.append("--network-only")
    if args.nat_check:
        command.append("--nat-check")
    if args.nat_only:
        command.append("--nat-only")
    if args.nat_single_peer:
        command.append("--nat-single-peer")
    if args.stun_only:
        command.append("--stun-only")
    if args.faults:
        command.append("--faults")
    if args.second_publisher:
        command += ["--second-publisher", args.second_publisher]
    if args.resources:
        command.append("--resources")
    if args.tcp:
        command.append("--tcp")
    if args.tcp_only:
        command.append("--tcp-only")
    print(f"Artifacts: {directory}", flush=True)
    child = subprocess.Popen(command)
    try:
        status = child.wait()
    except KeyboardInterrupt:
        child.terminate()
        try:
            status = child.wait(timeout=10)
        except subprocess.TimeoutExpired:
            child.kill()
            status = child.wait()
    after = host_snapshot()
    (directory / "host-after.json").write_text(json.dumps(after, indent=2) + "\n")
    unchanged = stable_snapshot(before) == stable_snapshot(after) and os.readlink("/proc/self/ns/net") == original_namespace
    (directory / "host-check.json").write_text(json.dumps({"unchanged": unchanged}, indent=2) + "\n")
    result_path = directory / "result.json"
    result = json.loads(result_path.read_text()) if result_path.exists() else {"pass": False, "error": "Isolated runner did not complete"}
    result.update(host_unchanged=unchanged, launcher_exit=status)
    result["pass"] = result["pass"] and unchanged and status == 0
    result_path.write_text(json.dumps(result, indent=2) + "\n")
    if not unchanged:
        print("FAIL: main network namespace snapshot changed during the run", file=sys.stderr)
        return 1
    print(f"Main network namespace unchanged; exit={status}", flush=True)
    return status


if __name__ == "__main__":
    sys.exit(main())
