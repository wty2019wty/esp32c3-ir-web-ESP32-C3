#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
api-demo.py - esp32c3-ir-web 演示脚本（WebSocket-only，无 REST API）

流程（全部走 ws://<host>/api/ws）：
  1. 连接 /api/ws，发送 {"type":"login","user":"...","pass":"..."} 登录
     （REST /api/login 已移除，登录是 WS 上唯一无需 token 的操作；
     成功后该连接即为已认证会话）
  2. 通过命令通道执行 status / play：
     发送 {"type":"cmd","id":N,"cmd":cmd,"body":{...}}
     收到 {"type":"resp","id":N,"ok":true,"data":{...}}
  3. 监听 status / frame 推送（收到 status 需回 ack）
  4. 通过 logout 命令退出（服务端回复后关闭连接）

说明：服务端在状态无变化时每 20 秒也会推送一次 status 心跳（仍需回 ack），
  因此长时间监听不会因 NAT 空闲回收而掉线；若 45 秒内收不到任何消息说明连接已死。

用法示例：
  python api-demo.py                                  # 默认连 192.168.4.1，admin/admin
  python api-demo.py --host 192.168.1.50 --user admin --password xxxx
  python api-demo.py --hxd ED127F80 --freq 38000
  python api-demo.py --listen 8                       # 发完信号后观察 WS 推送并 ack
  python api-demo.py --listen 0                       # 发完信号后立即退出
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


def ws_send_close(sock, code=1000):
    """发送 CLOSE 帧（标准断开握手，避免服务器端报 recv 错误）。"""
    payload = struct.pack(">H", code)
    mask = os.urandom(4)
    header = bytearray([0x88])  # FIN + CLOSE
    header.append(0x80 | len(payload))
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
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


def ws_call(sock, msg, timeout=10):
    """发送一条 WS 消息并读取下一条响应消息（用于 login 这类无 id 的应答）。"""
    ws_send_text(sock, json.dumps(msg))
    sock.settimeout(timeout)
    while True:
        opcode, payload = ws_recv_frame(sock)
        if opcode == 0x8:
            raise ConnectionError("服务端 CLOSE")
        if opcode != 0x1:
            continue
        try:
            return json.loads(payload.decode("utf-8"))
        except Exception:
            continue


_ws_rpc_id = 0


def ws_rpc(sock, cmd, body=None, timeout=10):
    """通过 WebSocket 命令通道执行一个命令，返回响应 data（dict）。

    发送 {"type":"cmd","id":N,"cmd":cmd,"body":body}，等待匹配的
    {"type":"resp","id":N,...}；中间的 status/frame 推送会被跳过。
    失败抛 RuntimeError / TimeoutError / ConnectionError。
    """
    global _ws_rpc_id
    _ws_rpc_id += 1
    rid = _ws_rpc_id
    ws_send_text(sock, json.dumps({
        "type": "cmd", "id": rid, "cmd": cmd, "body": body or {},
    }))
    sock.settimeout(timeout)
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            opcode, payload = ws_recv_frame(sock)
        except socket.timeout:
            break
        if opcode == 0x8:
            raise ConnectionError("服务端 CLOSE")
        if opcode != 0x1:
            continue
        try:
            msg = json.loads(payload.decode("utf-8"))
        except Exception:
            continue
        if msg.get("type") == "resp" and msg.get("id") == rid:
            if msg.get("ok"):
                return msg.get("data", {})
            raise RuntimeError(msg.get("error", "命令失败"))
        # 其他类型（status/frame 推送）：顺手 ack status，避免服务端每秒补发
        if msg.get("type") == "status" and msg.get("id") is not None:
            ws_send_text(sock, json.dumps({"type": "ack", "id": msg["id"]}))
    raise TimeoutError(f"WS 命令 {cmd} 超时")


def ws_only_demo(host, port, user, password, payload, listen_secs):
    """纯 WS 流程：login -> status -> play -> 监听推送 -> logout。"""
    print(f"[*] 连接 ws://{host}:{port}/api/ws 并登录 ...")
    sock = ws_connect(host, port)
    try:
        r = ws_call(sock, {"type": "login", "user": user, "pass": password})
        if not r.get("ok"):
            err = r.get("error", "unknown")
            if err == "too many attempts":
                print(f"[x] 登录被限速，请 {r.get('retry_after', 30)} 秒后再试")
            else:
                print(f"[x] WS 登录失败: {err}")
            return
        token = r.get("token", "")
        if r.get("must_change_pwd"):
            print("[!] 注意：设备仍在使用默认凭据，Web 页面会强制要求修改密码")
        print(f"[+] WS 登录成功，token={token[:8]}...")

        st = ws_rpc(sock, "status")
        print(f"[*] WS status: 模式={st.get('mode')} "
              f"STA={st.get('sta_ip') or '-'} 载波={st.get('carrier_hz')}Hz")

        print(f"[*] WS play: {json.dumps(payload, ensure_ascii=False)}")
        ws_rpc(sock, "play", payload)
        print("[+] WS 回放命令已接受")

        if listen_secs > 0:
            print(f"[*] 监听推送 {listen_secs} 秒（Ctrl+C 结束）...")
            sock.settimeout(1.0)
            deadline = time.time() + listen_secs
            while time.time() < deadline:
                try:
                    opcode, frame_payload = ws_recv_frame(sock)
                except socket.timeout:
                    continue
                except (ConnectionError, OSError) as e:
                    print(f"[*] 连接已断开: {e}")
                    break
                if opcode == 0x8:
                    print("[*] 服务端发送 CLOSE")
                    break
                if opcode != 0x1:
                    continue
                try:
                    msg = json.loads(frame_payload.decode("utf-8"))
                except Exception:
                    continue
                t = msg.get("type")
                if t == "status":
                    sid = msg.get("id")
                    print(f"[*] WS status id={sid}: "
                          f"播放={'是' if msg.get('data', {}).get('playing') else '否'}")
                    if sid is not None:
                        ws_send_text(sock, json.dumps({"type": "ack", "id": sid}))
                elif t == "frame":
                    print(f"[*] WS frame seq={msg.get('data', {}).get('seq')}")

        print("[*] WS logout（响应后服务端关闭连接）...")
        try:
            ws_rpc(sock, "logout", timeout=3)
            print("[+] WS logout 完成")
        except (RuntimeError, TimeoutError, ConnectionError, OSError) as e:
            print(f"[*] logout 后连接关闭: {e}")
    except (RuntimeError, TimeoutError, ConnectionError, OSError) as e:
        print(f"[x] WS 失败: {e}")
    finally:
        try:
            ws_send_close(sock)
        except OSError:
            pass
        sock.close()


def main():
    parser = argparse.ArgumentParser(description="esp32c3-ir-web 演示（WS 登录 + 发送红外 + WS 推送，无 REST）")
    parser.add_argument("--host", default="192.168.4.1", help="设备地址（默认 192.168.4.1）")
    parser.add_argument("--port", type=int, default=80, help="HTTP 端口（默认 80）")
    parser.add_argument("--user", default="admin", help="登录用户名（默认 admin）")
    parser.add_argument("--password", default="admin", help="登录密码（默认 admin）")
    parser.add_argument("--hxd", default="ED127F80", help="NEC hxd 码（默认 ED127F80）")
    parser.add_argument("--freq", type=int, default=38000, help="载波频率 Hz（默认 38000）")
    parser.add_argument("--raw", default=None,
                        help="原始数据文本（Frequency: ... 行 + 逗号序列），与 --hxd 二选一，优先于 --hxd")
    parser.add_argument("--listen", type=int, default=5, help="发完信号后监听推送秒数（默认 5，0 = 不监听）")
    parser.add_argument("--timeout", type=int, default=10, help="WS 超时秒数（默认 10）")
    args = parser.parse_args()

    host = args.host
    if host.startswith("http://"):
        host = host[len("http://"):]

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

    ws_only_demo(host, args.port, args.user, args.password, payload, args.listen)
    print("[+] 完成")


if __name__ == "__main__":
    main()
