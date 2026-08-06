#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
api-demo.py - esp32c3-ir-web API 演示脚本

REST 流程：
  1. POST /api/login 登录，获取 session token
  2. GET  /api/status 验证 token 有效（打印设备状态）
  3. POST /api/play 发送 NEC hxd（默认 ED127F80，载波 38000 Hz）

可选（--ws）：发送后连接 /api/ws，演示 WebSocket 推送协议：
  - 发送 {"type":"auth","token":...} 认证
  - 收到 {"type":"status","id":N,...} 后回发 {"type":"ack","id":N} 确认抄收
  - 打印 status / frame 推送，默认监听 5 秒

用法示例：
  python api-demo.py                                  # 默认连 192.168.4.1，admin/admin
  python api-demo.py --host 192.168.1.50 --user admin --password xxxx
  python api-demo.py --hxd ED127F80 --freq 38000
  python api-demo.py --ws --listen 8                  # 发完信号后观察 WS 推送并 ack
  python api-demo.py --raw "Frequency: 38000 Hz

9000, 4500, 560, 560, 1690"
"""

import argparse
import base64
import hashlib
import json
import os
import re
import socket
import struct
import sys
import time
import urllib.error
import urllib.request


def http_json(method, url, token=None, body=None, timeout=10):
    """发起 HTTP JSON 请求，返回 (status, dict)。"""
    req = urllib.request.Request(url, method=method)
    req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("X-Auth-Token", token)
    data = json.dumps(body).encode("utf-8") if body is not None else None
    try:
        with urllib.request.urlopen(req, data=data, timeout=timeout) as resp:
            raw = resp.read().decode("utf-8") or "{}"
            return resp.status, json.loads(raw)
    except urllib.error.HTTPError as e:
        try:
            detail = json.loads(e.read().decode("utf-8") or "{}")
        except Exception:
            detail = {}
        return e.code, detail
    except urllib.error.URLError as e:
        print(f"[x] 无法连接设备: {e.reason}")
        sys.exit(1)


def login(base, user, password):
    print(f"[*] 登录 {base}/api/login (user={user}) ...")
    status, d = http_json("POST", base + "/api/login",
                          body={"user": user, "pass": password})
    if status == 200 and d.get("token"):
        if d.get("must_change_pwd"):
            print("[!] 注意：设备仍在使用默认凭据，Web 页面会强制要求修改密码")
        print("[+] 登录成功，已获取 token")
        return d["token"]
    if status == 429:
        print(f"[x] 登录被限速，请 {d.get('retry_after', 30)} 秒后再试")
    elif status == 401:
        print("[x] 用户名或密码错误")
    else:
        print(f"[x] 登录失败 HTTP {status}: {d}")
    sys.exit(1)


def show_status(base, token):
    status, d = http_json("GET", base + "/api/status", token=token)
    if status != 200:
        print(f"[x] 获取状态失败 HTTP {status}: {d}")
        sys.exit(1)
    mode = d.get("mode", "?")
    ips = []
    if d.get("ap_ip"):
        ips.append(f"AP {d.get('ap_ssid', '')} {d['ap_ip']}")
    if d.get("sta_ip"):
        ips.append(f"STA {d.get('sta_ssid', '')} {d['sta_ip']}")
    print(f"[*] 设备状态: 模式={mode}" + (f", {' / '.join(ips)}" if ips else ""))


def parse_raw_text(text):
    """解析 Web 页面原始数据格式: 'Frequency: 38000 Hz' + 逗号分隔微秒序列。"""
    freq = 38000
    nums = []
    for line in text.splitlines():
        m = re.match(r"^\s*Frequency:\s*(\d+)", line, re.IGNORECASE)
        if m:
            freq = int(m.group(1))
            continue
        for part in line.split(","):
            v = part.strip()
            if v.isdigit():
                nums.append(int(v))
    return freq, nums


def send_play(base, token, payload):
    print(f"[*] 发送 /api/play: {json.dumps(payload, ensure_ascii=False)}")
    status, d = http_json("POST", base + "/api/play", token=token, body=payload)
    if status == 200 and d.get("ok"):
        print("[+] 回放请求已接受")
    else:
        print(f"[x] 回放失败 HTTP {status}: {d}")
        sys.exit(1)


# ---------------- WebSocket 客户端（纯标准库） ----------------

def _recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("连接被关闭")
        buf += chunk
    return buf


def ws_connect(host, port, path="/api/ws", timeout=10):
    """RFC6455 握手，返回已升级为 WebSocket 的 TCP socket。"""
    sock = socket.create_connection((host, port), timeout=timeout)
    key = base64.b64encode(os.urandom(16)).decode("ascii")
    request = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n"
    )
    sock.sendall(request.encode("ascii"))
    response = b""
    while b"\r\n\r\n" not in response:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("WebSocket 握手时连接被关闭")
        response += chunk
    header, _, _ = response.partition(b"\r\n\r\n")
    lines = header.split(b"\r\n")
    status_line = lines[0].decode("ascii", "replace")
    if "101" not in status_line:
        sock.close()
        raise ConnectionError(f"WebSocket 握手失败: {status_line}")
    expected = base64.b64encode(
        hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()
    ).decode("ascii")
    for line in lines[1:]:
        if line.lower().startswith(b"sec-websocket-accept:"):
            if line.split(b":", 1)[1].strip().decode("ascii") != expected:
                sock.close()
                raise ConnectionError("Sec-WebSocket-Accept 校验失败")
            break
    return sock


def ws_send_text(sock, text):
    """发送一个完整的文本帧（客户端帧必须掩码）。"""
    data = text.encode("utf-8")
    mask = os.urandom(4)
    header = bytearray([0x81])  # FIN + TEXT
    length = len(data)
    if length < 126:
        header.append(0x80 | length)
    elif length < 65536:
        header.append(0x80 | 126)
        header += bytearray(struct.pack(">H", length))
    else:
        header.append(0x80 | 127)
        header += bytearray(struct.pack(">Q", length))
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
    sock.sendall(bytes(header) + mask + masked)


def ws_recv_frame(sock):
    """接收一个完整的服务器帧（服务器帧不掩码），返回 (opcode, payload)。"""
    b1, b2 = _recv_exact(sock, 2)
    opcode = b1 & 0x0F
    length = b2 & 0x7F
    if length == 126:
        length = struct.unpack(">H", _recv_exact(sock, 2))[0]
    elif length == 127:
        length = struct.unpack(">Q", _recv_exact(sock, 8))[0]
    mask = _recv_exact(sock, 4) if (b2 & 0x80) else None
    payload = _recv_exact(sock, length)
    if mask:
        payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    return opcode, payload


def ws_demo(host, port, token, listen_secs):
    """连接 /api/ws，认证后监听推送，收到 status 回 ack。"""
    print(f"[*] 连接 ws://{host}:{port}/api/ws ...")
    sock = ws_connect(host, port)
    try:
        ws_send_text(sock, json.dumps({"type": "auth", "token": token}))
        print("[*] 已发送认证消息，等待推送（Ctrl+C 结束）...")
        sock.settimeout(1.0)
        deadline = time.time() + listen_secs
        while time.time() < deadline:
            try:
                opcode, payload = ws_recv_frame(sock)
            except socket.timeout:
                continue
            if opcode == 0x8:  # CLOSE
                print("[*] 服务端发送 CLOSE")
                break
            if opcode != 0x1:  # 只处理文本帧
                continue
            try:
                msg = json.loads(payload.decode("utf-8"))
            except Exception:
                continue
            t = msg.get("type")
            if t == "auth":
                if msg.get("ok"):
                    print("[*] WS 认证成功")
                else:
                    print(f"[x] WS 认证失败: {msg.get('error')}")
                    break
            elif t == "status":
                sid = msg.get("id")
                data = msg.get("data", {})
                print(f"[*] WS status id={sid}: 模式={data.get('mode')} "
                      f"STA={data.get('sta_ip') or '-'} "
                      f"载波={data.get('carrier_hz')}Hz "
                      f"播放={'是' if data.get('playing') else '否'}")
                if sid is not None:
                    ws_send_text(sock, json.dumps({"type": "ack", "id": sid}))
            elif t == "frame":
                print(f"[*] WS frame seq={msg.get('data', {}).get('seq')}")
    except KeyboardInterrupt:
        print("\n[*] 用户中断")
    finally:
        sock.close()


def main():
    parser = argparse.ArgumentParser(description="esp32c3-ir-web API 演示（登录 + 发送红外 + WS 推送）")
    parser.add_argument("--host", default="192.168.4.1", help="设备地址（默认 192.168.4.1）")
    parser.add_argument("--port", type=int, default=80, help="HTTP 端口（默认 80）")
    parser.add_argument("--user", default="admin", help="登录用户名（默认 admin）")
    parser.add_argument("--password", default="admin", help="登录密码（默认 admin）")
    parser.add_argument("--hxd", default="ED127F80", help="NEC hxd 码（默认 ED127F80）")
    parser.add_argument("--freq", type=int, default=38000, help="载波频率 Hz（默认 38000）")
    parser.add_argument("--raw", default=None,
                        help="原始数据文本（Frequency: ... 行 + 逗号序列），与 --hxd 二选一，优先于 --hxd")
    parser.add_argument("--ws", action="store_true", help="发送后连接 WebSocket 演示推送与 ack")
    parser.add_argument("--listen", type=int, default=5, help="--ws 模式下监听秒数（默认 5）")
    parser.add_argument("--timeout", type=int, default=10, help="HTTP 超时秒数（默认 10）")
    args = parser.parse_args()

    host = args.host
    if host.startswith("http://"):
        host = host[len("http://"):]
    base = f"http://{host}:{args.port}"

    token = login(base, args.user, args.password)
    show_status(base, token)

    if args.raw is not None:
        freq, nums = parse_raw_text(args.raw)
        if not nums:
            print("[x] --raw 中未解析到任何微秒数据")
            sys.exit(1)
        payload = {"type": "raw", "freq": freq, "data": nums}
    else:
        if not re.fullmatch(r"[0-9a-fA-F]{1,8}", args.hxd):
            print("[x] --hxd 需为 1-8 位十六进制（如 ED127F80）")
            sys.exit(1)
        payload = {"type": "hxd", "value": args.hxd.upper(), "freq": args.freq}

    send_play(base, token, payload)
    if args.ws:
        ws_demo(host, args.port, token, args.listen)
    print("[+] 完成")


if __name__ == "__main__":
    main()
