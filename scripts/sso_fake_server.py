"""A local stand-in for Nexus's login service (wss://sso.nexusmods.com) for scripts/sso_selftest.ps1.

It speaks the SSO protocol (github.com/Nexus-Mods/sso-integration-demo) over plain ws:// on 127.0.0.1, so EMF's real
WinHTTP WebSocket client (src/nexus/sso.cpp) can be tested without an application slug and without contacting Nexus.

    python -I sso_fake_server.py <port file> <log file> <mode>

mode "plain": hello -> connection token -> the API key, sent in two fragments.
mode "drop":  the first connection closes after the token; the client must come back with the same id and the token,
              and then gets the key.
mode "hang":  the token, then nothing: the client's time limit must abort its waiting socket.
The port it listens on goes to <port file>; what each connection said (request line, User-Agent, hello) to <log file>.
"""
import base64
import hashlib
import json
import socket
import struct
import sys

GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
TOKEN = "fakeConnectionToken123"
KEY = "ZmFrZS1hcGkta2V5LWZvci1lbWY=--c2VsZnRlc3Q="


def read_exact(c, n):
    b = b""
    while len(b) < n:
        chunk = c.recv(n - len(b))
        if not chunk:
            raise EOFError
        b += chunk
    return b


def handshake(c):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = c.recv(4096)
        if not chunk:
            raise EOFError
        data += chunk
        if len(data) > 65536:
            raise ValueError("headers too long")
    lines = data.split(b"\r\n\r\n", 1)[0].decode("latin-1").split("\r\n")
    headers = {}
    for line in lines[1:]:
        if ":" in line:
            k, v = line.split(":", 1)
            headers[k.strip().lower()] = v.strip()
    accept = base64.b64encode(hashlib.sha1(headers["sec-websocket-key"].encode() + GUID).digest()).decode()
    c.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Accept: %s\r\n\r\n" % accept).encode())
    return lines[0], headers


def send_frame(c, op, payload, fin=True):
    n = len(payload)
    head = bytes([(0x80 if fin else 0) | op])
    if n < 126:
        head += bytes([n])
    elif n < 65536:
        head += bytes([126]) + struct.pack(">H", n)
    else:
        head += bytes([127]) + struct.pack(">Q", n)
    c.sendall(head + payload)


def recv_text(c):
    while True:
        b0, b1 = read_exact(c, 2)
        op, masked, n = b0 & 0x0F, b1 & 0x80, b1 & 0x7F
        if n == 126:
            n = struct.unpack(">H", read_exact(c, 2))[0]
        elif n == 127:
            n = struct.unpack(">Q", read_exact(c, 8))[0]
        mask = read_exact(c, 4) if masked else b"\0\0\0\0"
        payload = bytes(x ^ mask[i % 4] for i, x in enumerate(read_exact(c, n)))
        if op == 1:
            return payload.decode()
        if op == 8:
            raise EOFError
        if op == 9:
            send_frame(c, 0xA, payload)  # ping -> pong


def main():
    port_file, log_file, mode = sys.argv[1], sys.argv[2], sys.argv[3]
    server = socket.socket()
    server.bind(("127.0.0.1", 0))
    server.listen(4)
    server.settimeout(30)
    with open(port_file, "w") as f:
        f.write(str(server.getsockname()[1]))
    log = []
    try:
        for n in range(2 if mode == "drop" else 1):
            c, _ = server.accept()
            c.settimeout(30)
            request, headers = handshake(c)
            hello = json.loads(recv_text(c))
            log.append({"request": request, "user_agent": headers.get("user-agent", ""), "hello": hello})
            send_frame(c, 1, json.dumps({"success": True, "data": {"connection_token": TOKEN}, "error": None}).encode())
            if mode == "drop" and n == 0:
                c.close()  # dropped without a close frame: the client comes back with the token
                continue
            if mode == "hang":
                try:
                    recv_text(c)  # nothing more is sent; wait until the client gives up and goes away
                except Exception:
                    pass
                c.close()
                break
            key = json.dumps({"success": True, "data": {"api_key": KEY}, "error": None}).encode()
            half = len(key) // 2
            send_frame(c, 1, key[:half], fin=False)  # a text message in two fragments
            send_frame(c, 0, key[half:], fin=True)
            try:
                recv_text(c)  # until the client goes away
            except Exception:
                pass
            c.close()
    finally:
        with open(log_file, "w") as f:
            f.write(json.dumps(log))


main()
