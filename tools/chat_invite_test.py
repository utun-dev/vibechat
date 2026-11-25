#!/usr/bin/env python3
"""
chat_invite_test.py — headless test of the inviter-side invite flow.

Проверяет добавление сервера по его SERVER-ссылке и синхронизацию MEMBERS.
Обычное приглашение по полному Ed25519 доходит до того же сервера и отклоняется.

Usage:
    python3 tools/chat_invite_test.py
"""

import asyncio
import json
import os
from pathlib import Path
import secrets
import socket
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(__file__))
from chat_client import ChatClient, ChatClientError

UTUN_BIN = os.path.join(os.path.dirname(__file__), "..", "src", "utun")
READY_TIMEOUT = 3.0       # max wait for utun ready
SYNC_TIMEOUT = 10.0       # max wait for join + sync
REQUEST_TIMEOUT = 5.0     # per-request timeout
TOTAL_TIMEOUT = 30.0      # overall test timeout


def find_free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def write_config(path, etcp_port, ctrl_port, db_subdir):
    content = f"""[global]
db_path={db_subdir}

[ntp]
enabled=no

[debug]
member_sync=warn

[server: srv]
addr=127.0.0.1:{etcp_port}
type=public

[chatserver]
db_path={db_subdir}
headless_control_bind=127.0.0.1:{ctrl_port}
add_key={secrets.token_hex(32)}

[allowed_keys]
allow_all=1
"""
    with open(path, "w") as f:
        f.write(content)


async def kill_proc(proc, label):
    if proc is None or proc.returncode is not None:
        return
    try:
        proc.terminate()
        try:
            await asyncio.wait_for(proc.wait(), timeout=3.0)
        except asyncio.TimeoutError:
            proc.kill()
            await proc.wait()
    except ProcessLookupError:
        pass


async def wait_ready(cli, timeout=READY_TIMEOUT):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            await cli.ping()
            return True
        except ChatClientError:
            await asyncio.sleep(0.1)
    return False


def check(name, expr, detail=""):
    if not expr:
        detail = f" ({detail})" if detail else ""
        raise AssertionError(f"FAIL: {name}{detail}")
    print(f"  OK: {name}")


async def main():
    etcp_a = find_free_port()
    etcp_b = find_free_port()
    ctrl_a = find_free_port()
    ctrl_b = find_free_port()

    print(f"ports: etcp={etcp_a},{etcp_b}  ctrl={ctrl_a},{ctrl_b}")

    workspace = tempfile.TemporaryDirectory(prefix="utun_invite_test_")
    tmpdir = workspace.name
    db_a = os.path.join(tmpdir, "db_a")
    db_b = os.path.join(tmpdir, "db_b")
    os.makedirs(db_a, exist_ok=True)
    os.makedirs(db_b, exist_ok=True)

    config_a = os.path.join(tmpdir, "a.conf")
    config_b = os.path.join(tmpdir, "b.conf")
    write_config(config_a, etcp_a, ctrl_a, db_a)
    write_config(config_b, etcp_b, ctrl_b, db_b)

    proc_a = None
    proc_b = None

    try:
        print("\n--- Starting utun ---")
        log_a = os.path.join(tmpdir, "utun_a.log")
        log_b = os.path.join(tmpdir, "utun_b.log")
        pid_a = os.path.join(tmpdir, "utun_a.pid")
        pid_b = os.path.join(tmpdir, "utun_b.pid")

        proc_a = await asyncio.create_subprocess_exec(
            UTUN_BIN, "-f", "-p", pid_a, "-l", log_a, "-c", config_a,
            stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.DEVNULL,
        )
        proc_b = await asyncio.create_subprocess_exec(
            UTUN_BIN, "-f", "-p", pid_b, "-l", log_b, "-c", config_b,
            stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.DEVNULL,
        )

        print(f"  proc_a pid={proc_a.pid}  proc_b pid={proc_b.pid}")
        await asyncio.sleep(0.5)

        print("\n--- Connecting to headless control ---")
        async with ChatClient(port=ctrl_a, timeout=REQUEST_TIMEOUT) as cli_a:
            if not await wait_ready(cli_a):
                raise RuntimeError(f"node A not ready after {READY_TIMEOUT}s")
            print(f"  node A ready")

            async with ChatClient(port=ctrl_b, timeout=REQUEST_TIMEOUT) as cli_b:
                if not await wait_ready(cli_b):
                    raise RuntimeError(f"node B not ready after {READY_TIMEOUT}s")
                print(f"  node B ready")

                # ── Create channel on A ──
                print("\n--- Create channel ---")
                await cli_a.create_channel("TestGroup")
                channels = await cli_a.channels()
                check("channel created", isinstance(channels, list) and len(channels) == 1,
                      f"channels={json.dumps(channels)}")
                ch_id = str(channels[0]["id"])
                print(f"  channel_id={ch_id}  name={channels[0]['name']}")

                # ── Read the public address from the running instance ──
                print("\n--- Read B identity ---")
                b_node_id = (await cli_b.ping()).get("ed25519")
                if not b_node_id or len(b_node_id) != 64:
                    raise RuntimeError("failed to read B's Ed25519 address from running node")
                print(f"  B node_id={b_node_id}")

                print("\n--- Add B using its server invitation ---")
                server = await cli_b.server_link()
                resp = await cli_a.server_add(ch_id, server["link"])
                check("server addition started", resp.get("adding"))

                # ── Wait for B to join + member_sync ──
                print(f"\n--- Wait join + sync (max {SYNC_TIMEOUT}s) ---")
                deadline = time.monotonic() + SYNC_TIMEOUT
                member_count_b = 0
                members_b = []
                while time.monotonic() < deadline:
                    try:
                        members_b = await cli_b.members(ch_id)
                        member_count_b = len(members_b) if isinstance(members_b, list) else 0
                        if member_count_b >= 2:
                            break
                    except ChatClientError:
                        pass
                    await asyncio.sleep(0.15)

                print(f"\n--- Members on B ({member_count_b}) ---")
                for m in (members_b if isinstance(members_b, list) else []):
                    print(f"  {m['node_id']} name={m.get('name','?')} online={m.get('online')} connected={m.get('connected')}")
                check("2 members on B (B joined A)", member_count_b >= 2,
                      f"got {member_count_b} after {SYNC_TIMEOUT}s")

                await asyncio.sleep(0.5)

                members_a = await cli_a.members(ch_id)
                print(f"\n--- Members on A ({len(members_a) if isinstance(members_a, list) else '?'}) ---")
                for m in members_a:
                    print(f"  {m['node_id']} name={m.get('name','?')} online={m.get('online')} connected={m.get('connected')}")
                check("2 members on A (B visible to A)", isinstance(members_a, list) and len(members_a) >= 2,
                      f"got {len(members_a) if isinstance(members_a, list) else '?'}")

                channels_b = await cli_b.channels()
                check("channel present on B", isinstance(channels_b, list) and len(channels_b) >= 1,
                      f"channels_b={json.dumps(channels_b)}")

                second = (await cli_a.create_channel("AddressOnly"))["channel_id"]
                resp = await cli_a.invite_to(second, b_node_id, f"127.0.0.1:{etcp_b}", proto=1)
                check("full Ed25519 accepted", resp.get("inviting") and resp.get("ed25519") == b_node_id)
                marker = f"CHANNEL_INVITE refused server group={second} "
                deadline = time.monotonic() + SYNC_TIMEOUT
                while marker not in Path(log_b).read_text() and time.monotonic() < deadline:
                    await asyncio.sleep(0.1)
                check("ordinary invitation reached server", marker in Path(log_b).read_text())
                check("server kept the authorized group only", [str(c["id"]) for c in await cli_b.channels()] == [ch_id])

        print("\n=== TEST PASSED ===")
        return 0

    except Exception as e:
        print(f"\n=== TEST FAILED: {e} ===", file=sys.stderr)
        import traceback
        traceback.print_exc()
        print(f"\n--- logs ---", file=sys.stderr)
        for lp in ["utun_a.log", "utun_b.log"]:
            lp = os.path.join(tmpdir, lp)
            if os.path.exists(lp):
                print(f"\n===== {lp} (tail) =====", file=sys.stderr)
                try:
                    with open(lp, "r") as f:
                        lines = f.readlines()
                        for ln in lines[-80:]:
                            sys.stderr.write(ln)
                except OSError:
                    pass
        return 1

    finally:
        print("\n--- Cleanup ---")
        await kill_proc(proc_a, "proc_a")
        await kill_proc(proc_b, "proc_b")

        workspace.cleanup()
        print(f"  temp dir cleaned: {tmpdir}")


if __name__ == "__main__":
    async def _run():
        try:
            return await asyncio.wait_for(main(), timeout=TOTAL_TIMEOUT)
        except asyncio.TimeoutError:
            print(f"\n=== TEST FAILED: total timeout {TOTAL_TIMEOUT}s ===", file=sys.stderr)
            return 1
    sys.exit(asyncio.run(_run()))
