#!/usr/bin/env python3
"""
chat_integration_test.py — integration test: 2 chat nodes over UDP, sync messages.

Запускает два процесса utun в фоне, создаёт канал, пишет 4 сообщения,
подключает второй узел по invite-ссылке, проверяет members и messages.

Usage:
    python3 tools/chat_integration_test.py
"""

import asyncio
import json
import os
import socket
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(__file__))
from chat_client import ChatClient, ChatClientError

UTUN_BIN = os.path.join(os.path.dirname(__file__), "..", "src", "utun")
READY_TIMEOUT = 3.0       # max wait for utun ready
SYNC_TIMEOUT = 8.0        # max wait for join + sync
REQUEST_TIMEOUT = 5.0     # per-request timeout
TOTAL_TIMEOUT = 25.0      # overall test timeout


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

[server: srv]
addr=127.0.0.1:{etcp_port}
type=public

[chatserver]
db_path={db_subdir}
headless_control_bind=127.0.0.1:{ctrl_port}

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

    tmpdir = tempfile.mkdtemp(prefix="utun_chat_test_")
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
        # ── Start utun processes ──
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

        # ── Open connections ──
        print("\n--- Connecting to headless control ---")
        async with ChatClient(port=ctrl_a, timeout=REQUEST_TIMEOUT) as cli_a:
            if not await wait_ready(cli_a):
                raise RuntimeError(f"node A not ready after {READY_TIMEOUT}s")
            print(f"  node A ready")

            async with ChatClient(port=ctrl_b, timeout=REQUEST_TIMEOUT) as cli_b:
                if not await wait_ready(cli_b):
                    raise RuntimeError(f"node B not ready after {READY_TIMEOUT}s")
                print(f"  node B ready")

                # ── Create channel ──
                print("\n--- Create channel ---")
                await cli_a.create_channel("TestGroup")
                channels = await cli_a.channels()
                check("channel created", isinstance(channels, list) and len(channels) == 1,
                      f"channels={json.dumps(channels)}")
                ch_id = str(channels[0]["id"])
                print(f"  channel_id={ch_id}  name={channels[0]['name']}")

                # ── Write 4 messages ──
                print("\n--- Write messages ---")
                for i in range(4):
                    await cli_a.send(ch_id, f"Message {i+1}")

                msgs_a = await cli_a.messages(ch_id, count=10)
                check("4 messages on A", isinstance(msgs_a, list) and len(msgs_a) == 4,
                      f"got {len(msgs_a) if isinstance(msgs_a, list) else '?'}")
                for i, m in enumerate(msgs_a):
                    print(f"  [#{i+1}] ts={m.get('ts','?')} author={m.get('author_id','?')}")

                # ── Invite ──
                print("\n--- Invite link ---")
                invite = await cli_a.invite(ch_id)
                link = invite.get("link", "")
                check("invite link", isinstance(link, str) and link.startswith("utun://"),
                      f"link={'...' + link[-20:] if link else 'MISSING'}")
                print(f"  link={link[:80]}...")

                # ── Connect node B ──
                print("\n--- Connect node B ---")
                conn = await cli_b.connect_channel(link)
                check("connect accepted", isinstance(conn, dict) and conn.get("connecting"),
                      f"resp={json.dumps(conn)}")
                print(f"  connecting={conn.get('connecting')}")

                # Wait for join + sync
                print(f"\n--- Wait sync (max {SYNC_TIMEOUT}s) ---")
                deadline = time.monotonic() + SYNC_TIMEOUT
                member_count = 0
                while time.monotonic() < deadline:
                    try:
                        members = await cli_b.members(ch_id)
                        member_count = len(members) if isinstance(members, list) else 0
                        if member_count >= 2:
                            break
                    except ChatClientError:
                        pass
                    await asyncio.sleep(0.15)
                check("2 members on B", member_count >= 2,
                      f"got {member_count} members after {SYNC_TIMEOUT}s")

                # Small pause to let db_sync fully settle
                await asyncio.sleep(0.5)

                # ── Verify members on B ──
                members = await cli_b.members(ch_id)
                print(f"\n--- Members on B ({len(members)}) ---")
                for m in members:
                    connected = "✓" if m.get("connected") else "✗"
                    online = "✓" if m.get("online") else "✗"
                    print(f"  {m['node_id']} name={m.get('name','?')} online={online} connected={connected}")
                check("owner online", any(m.get("online") for m in members),
                      f"members={json.dumps(members)}")
                check("owner connected", any(m.get("connected") for m in members),
                      f"members={json.dumps(members)}")

                # ── Verify messages on B ──
                if proc_b.returncode is not None:
                    raise RuntimeError(f"node B died with code {proc_b.returncode}")
                print(f"\n--- Messages on B ---")
                try:
                    msgs_b = await cli_b.messages(ch_id, count=10)
                except ChatClientError as e:
                    print(f"  messages error: {e}")
                    print(f"  proc_b.returncode={proc_b.returncode}")
                    raise
                check("4 messages on B", isinstance(msgs_b, list) and len(msgs_b) == 4,
                      f"got {len(msgs_b) if isinstance(msgs_b, list) else '?'}")
                for i, m in enumerate(msgs_b):
                    print(f"  [#{i+1}] ts={m.get('ts','?')} author={m.get('author_id','?')}")

                # ── Verify members on A ──
                members_a = await cli_a.members(ch_id)
                print(f"\n--- Members on A ({len(members_a)}) ---")
                for m in members_a:
                    print(f"  {m['node_id']} name={m.get('name','?')} online={m.get('online')} connected={m.get('connected')}")
                check("2 members on A", isinstance(members_a, list) and len(members_a) >= 2,
                      f"got {len(members_a) if isinstance(members_a, list) else '?'}")

        print("\n=== TEST PASSED ===")
        return 0

    except Exception as e:
        print(f"\n=== TEST FAILED: {e} ===", file=sys.stderr)
        import traceback
        traceback.print_exc()
        return 1

    finally:
        print("\n--- Cleanup ---")
        await kill_proc(proc_a, "proc_a")
        await kill_proc(proc_b, "proc_b")

        for f in [config_a, config_b]:
            try: os.unlink(f)
            except OSError: pass
        try: os.rmdir(db_a)
        except OSError: pass
        try: os.rmdir(db_b)
        except OSError: pass
        try: os.rmdir(tmpdir)
        except OSError: pass

        print(f"  temp dir cleaned: {tmpdir}")


if __name__ == "__main__":
    async def _run():
        try:
            return await asyncio.wait_for(main(), timeout=TOTAL_TIMEOUT)
        except asyncio.TimeoutError:
            print(f"\n=== TEST FAILED: total timeout {TOTAL_TIMEOUT}s ===", file=sys.stderr)
            return 1
    sys.exit(asyncio.run(_run()))
