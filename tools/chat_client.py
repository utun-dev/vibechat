"""
chat_client.py — async TCP JSON-RPC client for uTun headless chat control.

Usage:
    async with ChatClient(port=9999) as cli:
        await cli.create_channel("Test")
        channels = await cli.channels()
        await cli.send(channels[0]["id"], "hello")
"""

import asyncio
import json


class ChatClientError(Exception):
    """Error from chat client (timeout, refused, protocol, server error)."""


class ChatClient:
    def __init__(self, host="127.0.0.1", port=9999, timeout=5.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self._connected = False

    async def connect(self):
        self._connected = True

    async def disconnect(self):
        self._connected = False

    async def __aenter__(self):
        await self.connect()
        return self

    async def __aexit__(self, *args):
        await self.disconnect()

    # ── Internal: one-shot request ──

    async def _request(self, cmd, **params):
        if not self._connected:
            raise ChatClientError("not connected")

        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(self.host, self.port),
            timeout=self.timeout,
        )

        msg = {"id": 1, "cmd": cmd}
        msg.update(params)
        writer.write((json.dumps(msg, ensure_ascii=True) + "\n").encode())
        await writer.drain()

        try:
            line = await asyncio.wait_for(
                reader.readline(), timeout=self.timeout
            )
        except asyncio.TimeoutError:
            writer.close()
            raise ChatClientError(f"timeout waiting for '{cmd}' response")

        writer.close()
        if not line:
            raise ChatClientError(f"empty response for '{cmd}'")

        try:
            obj = json.loads(line.decode("utf-8", errors="replace"))
        except json.JSONDecodeError:
            raw = line.decode("utf-8", errors="replace")
            raise ChatClientError(f"invalid JSON for '{cmd}' len={len(raw)} tail=...{raw[-50:] if len(raw)>50 else raw}")

        if not obj.get("ok"):
            raise ChatClientError(obj.get("error", "unknown error"))

        return obj.get("data", {})

    # ── Commands ──

    async def ping(self):
        return await self._request("ping")

    async def status(self):
        return await self._request("status")

    async def channels(self):
        return await self._request("channels")

    async def members(self, ch_id):
        return await self._request("members", ch=str(ch_id))

    async def messages(self, ch_id, count=20, offset=0):
        return await self._request(
            "messages", ch=str(ch_id), count=int(count), offset=int(offset)
        )

    async def send(self, ch_id, text):
        return await self._request(
            "send", ch=str(ch_id), content_type="text", data=str(text)
        )

    async def invite(self, ch_id):
        return await self._request("invite", ch=str(ch_id))

    async def connect_channel(self, link):
        return await self._request("connect", link=str(link))

    async def create_channel(self, name):
        return await self._request("create_channel", name=str(name))

    async def server_link(self):
        return await self._request("server_link")

    async def server_add(self, ch_id, link):
        return await self._request("server_add", ch=str(ch_id), link=link)

    async def invite_to(self, ch_id, node_id, addr, proto=1):
        return await self._request(
            "invite_to", ch=str(ch_id), ed25519=str(node_id),
            addr=str(addr), proto=int(proto),
        )
