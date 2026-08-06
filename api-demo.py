#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
api-demo.py - esp32c3-ir-web API 演示脚本

流程：
  1. POST /api/login 登录，获取 session token
  2. GET  /api/status 验证 token 有效（打印设备状态）
  3. POST /api/play 发送 NEC hxd（默认 ED127F80，载波 38000 Hz）

可选：--raw 传入 Web 页面"原始数据"格式文本（Frequency: 38000 Hz + 逗号分隔微秒序列），
与 --hxd 二选一（传了 --raw 则优先发送原始数据）。

用法示例：
  python api-demo.py                                  # 默认连 192.168.4.1，admin/admin
  python api-demo.py --host 192.168.1.50 --user admin --password xxxx
  python api-demo.py --hxd ED127F80 --freq 38000
  python api-demo.py --raw "Frequency: 38000 Hz

9000, 4500, 560, 560, 1690"
"""

import argparse
import json
import re
import sys
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


def main():
    parser = argparse.ArgumentParser(description="esp32c3-ir-web API 演示（登录 + 发送红外）")
    parser.add_argument("--host", default="192.168.4.1", help="设备地址（默认 192.168.4.1）")
    parser.add_argument("--port", type=int, default=80, help="HTTP 端口（默认 80）")
    parser.add_argument("--user", default="admin", help="登录用户名（默认 admin）")
    parser.add_argument("--password", default="admin", help="登录密码（默认 admin）")
    parser.add_argument("--hxd", default="ED127F80", help="NEC hxd 码（默认 ED127F80）")
    parser.add_argument("--freq", type=int, default=38000, help="载波频率 Hz（默认 38000）")
    parser.add_argument("--raw", default=None,
                        help="原始数据文本（Frequency: ... 行 + 逗号序列），与 --hxd 二选一，优先于 --hxd")
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
    print("[+] 完成")


if __name__ == "__main__":
    main()
