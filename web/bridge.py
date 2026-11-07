#!/usr/bin/env python3
"""Web bridge for the C++ chat server.

Serves the browser UI (index.html) and relays each browser's WebSocket to the chat
server's length-prefixed TCP protocol, so a plain web page can join the same room as
the console clients. Standard library only (Python 3.8+); no pip install needed.

    python bridge.py --chat-port 9090                 # this PC only: http://127.0.0.1:8000
    python bridge.py --chat-port 9090 --bind 0.0.0.0  # let others on your network connect
"""
import argparse
import asyncio
import base64
import hashlib
import json
import os
import re
import socket
import struct
from urllib.parse import urlparse

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MAX_CHAT_FRAME = 64 * 1024          # matches MAX_MESSAGE_SIZE in common.h
MAX_WS_PAYLOAD = 128 * 1024
HEARTBEAT_TIMEOUT = 15              # seconds of silence from the chat server = dead
SERVER_HELLO = "CHAT/1 HELLO"
PING, PONG, USERS = "\x01PING", "\x01PONG", "\x01USERS"
HERE = os.path.dirname(os.path.abspath(__file__))


# --------------------------------------------------------------------------- chat side
async def read_chat_frame(reader):
    (length,) = struct.unpack(">I", await reader.readexactly(4))
    if length > MAX_CHAT_FRAME:
        raise ValueError("oversized frame")
    data = await reader.readexactly(length) if length else b""
    return data.decode("utf-8", "replace")


def write_chat_frame(writer, text):
    data = text.encode("utf-8")
    writer.write(struct.pack(">I", len(data)) + data)  # one write() so frames never interleave


# ----------------------------------------------------------------------- websocket side
class WebSocket:
    def __init__(self, reader, writer):
        self.reader, self.writer = reader, writer
        self._lock = asyncio.Lock()

    async def _send_frame(self, opcode, payload=b""):
        n = len(payload)
        if n < 126:
            head = struct.pack("!BB", 0x80 | opcode, n)
        elif n < 65536:
            head = struct.pack("!BBH", 0x80 | opcode, 126, n)
        else:
            head = struct.pack("!BBQ", 0x80 | opcode, 127, n)
        async with self._lock:
            self.writer.write(head + payload)
            await self.writer.drain()

    async def send_json(self, obj):
        await self._send_frame(0x1, json.dumps(obj).encode("utf-8"))

    async def close(self):
        try:
            await self._send_frame(0x8)
        except Exception:
            pass

    async def recv(self):
        """Next text message, or None when the browser closed the socket."""
        buf = b""
        while True:
            b0, b1 = await self.reader.readexactly(2)
            fin, opcode = b0 & 0x80, b0 & 0x0F
            length = b1 & 0x7F
            if length == 126:
                (length,) = struct.unpack("!H", await self.reader.readexactly(2))
            elif length == 127:
                (length,) = struct.unpack("!Q", await self.reader.readexactly(8))
            if not b1 & 0x80 or length > MAX_WS_PAYLOAD:
                return None                         # browsers always mask; refuse anything odd
            mask = await self.reader.readexactly(4)
            payload = bytearray(await self.reader.readexactly(length))
            for i in range(length):
                payload[i] ^= mask[i & 3]
            if opcode == 0x8:
                return None
            if opcode == 0x9:                       # ping -> pong
                await self._send_frame(0xA, bytes(payload))
            elif opcode in (0x1, 0x0):              # text / continuation
                buf += bytes(payload)
                if fin:
                    return buf.decode("utf-8", "replace")


# ------------------------------------------------------------------------ one chat user
async def run_session(ws, args):
    first = await ws.recv()
    try:
        join = json.loads(first or "")
        name = str(join["name"])[:40]
        assert join.get("type") == "join"
    except Exception:
        return

    try:
        creader, cwriter = await asyncio.wait_for(
            asyncio.open_connection(args.chat_host, args.chat_port), 5)
        if await asyncio.wait_for(read_chat_frame(creader), 5) != SERVER_HELLO:
            raise ConnectionError("not the chat server")
        write_chat_frame(cwriter, name)
        welcome = await asyncio.wait_for(read_chat_frame(creader), 5)
    except Exception:
        await ws.send_json({"type": "error",
                            "text": f"Could not reach the chat server at {args.chat_host}:{args.chat_port}."})
        return

    m = re.match(r"^Welcome, (.*)! Type /help", welcome, re.S)
    await ws.send_json({"type": "welcome", "name": m.group(1) if m else name, "text": welcome})

    async def chat_to_browser():
        while True:
            try:
                text = await asyncio.wait_for(read_chat_frame(creader), HEARTBEAT_TIMEOUT)
            except (asyncio.TimeoutError, asyncio.IncompleteReadError, ConnectionError, ValueError):
                await ws.send_json({"type": "lost"})
                return
            if text == PING:
                cwriter.write(struct.pack(">I", len(PONG)) + PONG.encode())
            elif text == "exit":
                await ws.send_json({"type": "ended"})
                return
            elif text.startswith(USERS):
                await ws.send_json({"type": "users", "list": [n for n in text.split("\t")[1:] if n]})
            elif not text.startswith("\x01"):
                await ws.send_json({"type": "msg", "text": text})

    async def browser_to_chat():
        while True:
            raw = await ws.recv()
            if raw is None:
                return
            try:
                msg = json.loads(raw)
            except ValueError:
                continue
            if msg.get("type") == "leave":
                return
            if msg.get("type") == "send":
                text = str(msg.get("text", "")).replace("\r", "").replace("\n", " ")
                if not text.strip():
                    continue
                if len(text.encode("utf-8")) > MAX_CHAT_FRAME:
                    await ws.send_json({"type": "msg", "text": "Message too long (max 64 KB), not sent."})
                    continue
                write_chat_frame(cwriter, text)
                await cwriter.drain()

    tasks = [asyncio.ensure_future(chat_to_browser()), asyncio.ensure_future(browser_to_chat())]
    try:
        await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
    finally:
        for t in tasks:
            t.cancel()
        try:
            write_chat_frame(cwriter, "exit")       # leave cleanly so the room is told right away
            await cwriter.drain()
        except Exception:
            pass
        cwriter.close()


# ------------------------------------------------------------------------------ HTTP
def http_response(status, body=b"", ctype="text/plain; charset=utf-8", extra=""):
    head = (f"HTTP/1.1 {status}\r\nContent-Type: {ctype}\r\nContent-Length: {len(body)}\r\n"
            f"Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
            f"Connection: close\r\n{extra}\r\n")
    return head.encode("latin-1") + body


async def handle_connection(reader, writer, args):
    try:
        head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 10)
        lines = head.decode("latin-1").split("\r\n")
        method, path, _ = lines[0].split(" ", 2)
        headers = {k.strip().lower(): v.strip()
                   for k, v in (l.split(":", 1) for l in lines[1:] if ":" in l)}

        if path == "/ws" and headers.get("upgrade", "").lower() == "websocket":
            origin = headers.get("origin")
            # Refuse pages from other sites (cross-site WebSocket hijacking of a local bridge).
            if origin and urlparse(origin).netloc != headers.get("host"):
                writer.write(http_response("403 Forbidden", b"Bad origin"))
                return
            key = headers.get("sec-websocket-key", "")
            accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
            writer.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                          f"Connection: Upgrade\r\nSec-WebSocket-Accept: {accept}\r\n\r\n").encode())
            await writer.drain()
            ws = WebSocket(reader, writer)
            try:
                await run_session(ws, args)
            finally:
                await ws.close()
        elif method == "GET" and path in ("/", "/index.html"):
            with open(os.path.join(HERE, "index.html"), "rb") as f:
                writer.write(http_response("200 OK", f.read(), "text/html; charset=utf-8"))
        else:
            writer.write(http_response("404 Not Found", b"Not found"))
        await writer.drain()
    except (asyncio.IncompleteReadError, asyncio.TimeoutError, ConnectionError, ValueError, OSError):
        pass
    finally:
        try:
            writer.close()
        except Exception:
            pass


def lan_address():
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("10.255.255.255", 1))     # no packet is sent; just picks the LAN interface
            return s.getsockname()[0]
    except OSError:
        return None


async def main():
    p = argparse.ArgumentParser(description="Web bridge for the C++ chat server")
    p.add_argument("--chat-host", default="127.0.0.1", help="chat server host (default 127.0.0.1)")
    p.add_argument("--chat-port", type=int, default=8080, help="chat server port (default 8080)")
    p.add_argument("--http-port", type=int, default=8000, help="web page port (default 8000)")
    p.add_argument("--bind", default="127.0.0.1",
                   help="address to listen on; use 0.0.0.0 so other devices can open the page")
    args = p.parse_args()

    server = await asyncio.start_server(lambda r, w: handle_connection(r, w, args), args.bind, args.http_port)
    print(f"Chat server expected at {args.chat_host}:{args.chat_port}")
    print(f"Web chat:  http://127.0.0.1:{args.http_port}")
    if args.bind == "0.0.0.0":
        ip = lan_address()
        if ip:
            print(f"On your network:  http://{ip}:{args.http_port}")
        print("Anyone who can reach that address can join the chat.")
    print("Press Ctrl+C to stop.")
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
