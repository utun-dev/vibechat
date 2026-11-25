#!/usr/bin/env python3

"""
mass_update.py - Mass update uTun servers via SSH
Usage: python3 mass_update.py [fast|full] [-c config] [-j jobs]  (default: fast)

Config format (JSON):
{
    "servers": [
        {"host": "10.0.0.1"},
        {"host": "10.0.0.2", "port": 2222, "user": "admin", "ssh_key": "keys/server2_rsa", "workdir": "/opt/utun"}
    ]
}

Default config: tools/utun_servers.json (in script directory)
Fallback: ~/.utun_servers.json
SSH keys: tools/keys/ (in script directory)
"""

import argparse
import json
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

SSH_TIMEOUT = 300


def get_script_dir():
    """Get script directory (tools/)."""
    return Path(__file__).parent.resolve()


def get_default_config():
    """Get config path: tools/utun_servers.json or fallback to home."""
    script_dir = get_script_dir()
    config_in_tools = script_dir / "utun_servers.json"
    if config_in_tools.exists():
        return config_in_tools
    config_in_home = Path.home() / ".utun_servers.json"
    if config_in_home.exists():
        return config_in_home
    return config_in_tools


def resolve_ssh_key(ssh_key, script_dir):
    """Resolve SSH key path."""
    if not ssh_key:
        return None
    key_path = Path(ssh_key)
    if key_path.is_absolute():
        return str(key_path)
    keys_dir = script_dir / "keys"
    return str(keys_dir / ssh_key)


def load_servers(config_path, script_dir):
    """Load server list from JSON config."""
    with open(config_path, 'r') as f:
        data = json.load(f)
    servers = data.get("servers", [])
    for server in servers:
        if "ssh_key" in server:
            server["_resolved_key"] = resolve_ssh_key(server["ssh_key"], script_dir)
    return servers


def update_server(server, command, logs_dir):
    """Run update on a single server via SSH. Returns result dict with logs_dir for log writing."""
    host = server["host"]
    port = server.get("port", 22)
    workdir = server.get("workdir", "~/utun")
    user = server.get("user", "root")
    resolved_key = server.get("_resolved_key")

    ssh_cmd = ["ssh"]
    if resolved_key and Path(resolved_key).exists():
        ssh_cmd.extend(["-i", resolved_key])
    ssh_cmd.extend([
        "-p", str(port),
        "-o", "StrictHostKeyChecking=no",
        "-o", "ConnectTimeout=10",
        "-o", "BatchMode=yes",
        f"{user}@{host}",
        f"cd {workdir} && ./tools/utun_update.sh {command}"
    ])

    try:
        result = subprocess.run(
            ssh_cmd,
            capture_output=True,
            text=True,
            timeout=SSH_TIMEOUT
        )
        return write_log({
            "host": host,
            "returncode": result.returncode,
            "stdout": result.stdout,
            "stderr": result.stderr
        }, logs_dir)
    except subprocess.TimeoutExpired:
        return write_log({
            "host": host,
            "returncode": -1,
            "stdout": "",
            "stderr": f"Timeout after {SSH_TIMEOUT}s"
        }, logs_dir)
    except Exception as e:
        return write_log({
            "host": host,
            "returncode": -1,
            "stdout": "",
            "stderr": str(e)
        }, logs_dir)


def write_log(result, logs_dir):
    """Write session log to file. Returns result dict."""
    host = result["host"]
    rc = result["returncode"]
    log_path = logs_dir / f"{host}.log"
    try:
        with open(log_path, 'w') as f:
            f.write(f"Host: {host}  |  Exit code: {rc}\n")
            f.write("=" * 60 + "\n")
            if result["stdout"]:
                f.write(result["stdout"])
                if not result["stdout"].endswith("\n"):
                    f.write("\n")
            if result["stderr"]:
                f.write("STDERR:\n")
                f.write(result["stderr"])
                if not result["stderr"].endswith("\n"):
                    f.write("\n")
        result["_log_file"] = str(log_path)
    except Exception as e:
        result["_log_file"] = f"ERROR writing log: {e}"
    return result


def main():
    script_dir = get_script_dir()

    parser = argparse.ArgumentParser(
        description="Mass update uTun servers via SSH"
    )
    parser.add_argument(
        "command",
        nargs='?',
        default="fast",
        choices=["fast", "full"],
        help="Update mode: fast (make) or full (autotools rebuild) (default: fast)"
    )
    parser.add_argument(
        "-c", "--config",
        help=f"Servers config file (default: auto from script dir)"
    )
    parser.add_argument(
        "-j", "--jobs",
        type=int,
        default=10,
        help="Parallel SSH jobs (default: 10)"
    )

    args = parser.parse_args()

    if args.config:
        config_path = Path(args.config)
    else:
        config_path = get_default_config()

    if not config_path.exists():
        print(f"ERROR: Config not found: {config_path}", file=sys.stderr)
        print("\nCreate config in script directory:", file=sys.stderr)
        print(f"  {script_dir / 'utun_servers.json'}", file=sys.stderr)
        print("\nOr use -c to specify custom config.", file=sys.stderr)
        print("\nFormat:", file=sys.stderr)
        example = {
            "servers": [
                {"host": "10.0.0.1"},
                {"host": "10.0.0.2", "port": 2222, "user": "admin", "ssh_key": "keys/server2_rsa"}
            ]
        }
        print(json.dumps(example, indent=2), file=sys.stderr)
        sys.exit(1)

    servers = load_servers(config_path, script_dir)
    if not servers:
        print("ERROR: No servers found in config", file=sys.stderr)
        sys.exit(1)

    logs_dir = Path("logs")
    logs_dir.mkdir(parents=True, exist_ok=True)

    print(f"Updating {len(servers)} servers in '{args.command}' mode...")
    print(f"Parallel jobs: {args.jobs}")
    print(f"Config: {config_path}")
    print(f"Logs: {logs_dir.resolve()}")
    print()

    success = 0
    failed = 0

    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = {
            executor.submit(update_server, s, args.command, logs_dir): s
            for s in servers
        }

        for future in as_completed(futures):
            result = future.result()
            host = result["host"]
            rc = result["returncode"]

            if rc == 0:
                success += 1
            else:
                failed += 1
                print(f"{'='*60}")
                print(f"Host: {host}  |  Exit code: {rc}  |  Log: {result.get('_log_file', 'N/A')}")
                print(f"{'='*60}")
                if result["stdout"]:
                    print(result["stdout"])
                if result["stderr"]:
                    print(result["stderr"], file=sys.stderr, end="")

    print(f"{'='*60}")
    print(f"Done: {success} succeeded, {failed} failed")

    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
