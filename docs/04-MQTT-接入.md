# MQTT 接入指南（可选）

设备内置 MQTT 客户端（`espressif/mqtt` 组件），复用与 WebSocket 完全相同的命令核心
与 JSON 序列化。**STA 连接路由器时才启动 MQTT**；纯 SoftAP（无外网/无到 Broker 路由）模式下
客户端保持停止。可接入 Home Assistant / Node-RED 等。

## 1. 启用与配置

MQTT 客户端代码**始终编译在固件中**；menuconfig 的 `IR_TOOL_MQTT_ENABLE`（或
`sdkconfig.defaults`）只决定 **Web 设置页"启用 MQTT"勾选框的初始值**（默认不勾选）。
实际启用与否完全由 Web 设置页控制：勾选并填好 Broker 地址保存后，设备重启即连接
（写入 NVS 并优先于 menuconfig 默认值）。

| 配置项 | 默认值 | 说明 |
|---|---|---|
| 启用 MQTT | 关 | `IR_TOOL_MQTT_ENABLE` 仅作 Web 勾选框默认值；代码始终编译 |
| Broker 地址 | 空（= 禁用） | 支持 `mqtt://`、`mqtts://`、`ws://`、`wss://` |
| 用户名 / 密码 | 空（匿名） | Broker 认证，密码只存不回显 |
| 客户端 ID | 空（自动） | 留空按 MAC 生成 `ir-web-XXXXXX` |
| MQTT 协议 | 3.1.1 | 可选 3.1.1 或 5.0，保存重启后生效 |
| TLS 证书校验 | 内置证书包 | 可选"内置证书包校验 / 跳过校验" |
| 四个主题 | `ir-web/*` | 见下方主题表 |
| QoS | 1 | 作用于订阅/命令/状态；**帧固定 QoS 0** |
| 推送帧 / 推送状态 | 开 | 两个独立开关 |
| 主题自动带设备标识 | 关 | 开启后主题嵌入 Client ID，多设备部署互不干扰 |

**传输方式（由 Broker 地址的 scheme 决定）**：

- `mqtt://192.168.1.100:1883`：MQTT over TCP（默认端口 1883）
- `mqtts://broker.example.com:8883`：MQTT over TLS（需证书校验或跳过校验）
- `ws://192.168.1.100:9001/mqtt`：MQTT over WebSocket（EMQX 常用端口 8083/9001，路径 `/mqtt`）
- `wss://broker.example.com:443/mqtt`：WebSocket + TLS

MQTT 的 WebSocket 是设备**出站客户端连接**，与设备内置 `/api/ws` WebSocket **服务端**
完全独立，端口、代码互不影响。

## 2. 主题一览

主题全部可在 Web 设置页修改，默认值如下。方向以**设备**为参考：

| 主题 | 默认值 | 设备方向 | 用途 |
|---|---|---|---|
| 命令主题 | `ir-web/cmd` | **订阅（收）** | 接收外部命令（JSON 信封） |
| 响应主题 | `ir-web/rsp` | **发送（发）** | 回传命令执行结果 |
| 状态主题 | `ir-web/status` | **发送（发）** | 发布状态 JSON；同时作为 LWT 离线主题 |
| 红外帧主题 | `ir-web/frame` | **发送（发）** | 每捕获一帧红外信号发布一帧 JSON |

即：设备**订阅 1 个主题**（命令），**发布 3 个主题**（响应/状态/帧）。外部客户端则相反：
要控制设备就**向 `ir-web/cmd` 发布**并**订阅 `ir-web/rsp`** 收响应；要监视就
订阅 `ir-web/status` 和 `ir-web/frame`。

**开启"主题自动带设备标识"后**，实际主题会在第一级路径后插入 Client ID
（Client ID 留空时按 MAC 自动生成，每台设备唯一）：

| 配置的主题 | 开启后的实际主题（示例 Client ID `esp-a1b2c3`） |
|---|---|
| `ir-web/cmd` | `ir-web/esp-a1b2c3/cmd` |
| `ir-web/rsp` | `ir-web/esp-a1b2c3/rsp` |
| `ir-web/status` | `ir-web/esp-a1b2c3/status` |
| `ir-web/frame` | `ir-web/esp-a1b2c3/frame` |

适合多台设备连接同一 Broker：命令、响应、状态（含 LWT）、红外帧都按设备隔离，
互不覆盖、互不串扰。开启后启动日志会打印四个实际主题，便于核对。

## 3. 命令协议

命令 JSON 信封与 WebSocket 完全一致（`cmd` / `body` 字段相同），额外支持可选 `id`
字段用于关联请求与响应：

```json
{"id":"a1","cmd":"status"}
{"id":"a2","cmd":"play","body":{"type":"hxd","value":"ED127F80","freq":38000}}
{"id":"a3","cmd":"frames","body":{"since":0}}
```

也支持直接发送裸命令名（如 `status`）。

### 支持的 cmd（发到 `ir-web/cmd`）

| cmd | body | 说明 |
|---|---|---|
| `status` | 空 | 返回当前状态对象 |
| `play` | `{"type":"hxd","value":"ED127F80"}` / `{"type":"raw","data":[...],"freq":N}` / `{"type":"frame","seq":N}`，可选 `freq` | 回放：NEC hxd / 原始数据 / 历史帧 |
| `carrier` | `{"freq":38000}` | 设置载波频率并持久化 |
| `rxpause` | `{"enabled":true}` | 回放时是否暂停接收 |
| `frames` | `{"since":N}` | 增量拉取帧历史；超 48KB 返回 `"truncated":true`，按 `last_seq` 继续拉取 |
| `fpub` | `{"enabled":true}` 或空 | **运行时**开关红外帧推送（不写 NVS，重启恢复 Web 设置页保存的「推送帧」配置）；缺省 `enabled` 时仅返回当前状态 `{"publish_frames":true}` |
| ~~`renew`~~ | 空 | **MQTT 通道禁用**（续期 Web 会话属会话敏感操作，MQTT 无会话故无用；仅 WebSocket 通道可执行） |
| ~~`wificfg`~~ / ~~`authcfg`~~ / ~~`webcfg`~~ / ~~`mqttcfg`~~ / ~~`wsorigin`~~ / ~~`logout`~~ | — | **MQTT 通道禁用**（配置/凭据/会话敏感命令，回复 `command not allowed on MQTT`；仅 WebSocket 通道可执行） |

### fpub 用法示例

```json
{"id":"p1","cmd":"fpub"}                            // 仅查询：返回当前状态
{"id":"p2","cmd":"fpub","body":{}}                  // 同上，仅查询
{"id":"p3","cmd":"fpub","body":{"enabled":true}}    // 开启帧推送
{"id":"p4","cmd":"fpub","body":{"enabled":false}}   // 关闭帧推送
fpub                                               // 裸命令名：仅查询
```

`fpub` 只改内存中的运行时常量、**不写 NVS**，断电/重启后恢复 Web 设置页保存的
「推送帧」配置；同时不受 `qos`、`publish_status` 等其它设置影响。与 WebSocket 推送
相互独立：该命令只控制 MQTT 帧推送，不影响 `/api/ws` 的帧推送。

### 响应格式（发布到 `ir-web/rsp`）

```json
{"ok":true,"id":"a1","cmd":"status","result":{...}}
{"ok":false,"id":"x","cmd":"play","error":"playback failed"}
```

- `ok`：执行结果；`id`/`cmd` 与请求对应，便于多命令并发时关联；
- `result`：成功时的返回数据（JSON 对象，即 WebSocket 响应的 `data`）；
- `error`：失败时的简短错误信息。

## 4. 状态推送（`ir-web/status`）

- 设备**连接成功时**发布一次完整状态（retained，新订阅者立即能拿到最新状态）；
- **播放开始/结束**时再次发布（`playing` 字段变化）；
- 设备**异常掉线**时，Broker 按 LWT 向该主题发布 `offline`（retained，覆盖旧状态）。

状态 JSON 字段：

```json
{
  "mode": "STA",
  "ap_ip": "",
  "sta_ip": "192.168.1.23",
  "ap_ssid": "",
  "sta_ssid": "MyWiFi",
  "sta_ip_mode": "dhcp",
  "sta_connected": true,
  "carrier_hz": 38000,
  "rx_pause_on_play": true,
  "playing": false
}
```

字段含义同 [WebSocket API](03-WebSocket-API.md#status状态有变化才推送) 的 status 数据。

## 5. 红外帧推送（`ir-web/frame`）

每捕获一帧红外信号立即发布一帧 JSON（与 WebSocket 推送的帧对象完全一致），
**固定 QoS 0**、由独立发布任务直接同步发送（QoS 0 的发布在调用任务内完成，
独立任务避免拖慢 IR 采集任务，尤其在 TLS broker 上），队列深度 4，
积压时丢弃新帧并限频告警（每秒最多一条日志）。运行时可用 `fpub` 命令随时
开/关本推送（见上文命令表，不写 NVS）：

```json
{
  "seq": 42,
  "ts": 1234567,
  "nec": {"ok":true,"repeat":false,"ext":false,"chksum":true,"bits":32,"addr":0,"cmd":127,"raw":3977412480,"hxd":"ED127F80"},
  "feat": {"total_us":67460,"pulses":32,"min_pulse":560,"max_pulse":1690,"leader_pulse":9000,"leader_space":4500,"last_gap":0,"seg_count":66},
  "freq": 38000,
  "durs": [9000,4500,560,560,560,1690,560,560]
}
```

字段说明见 [WebSocket API](03-WebSocket-API.md#frame新红外信号即时推送)。
NEC 解码失败时 `nec.ok=false`。示例中 `durs` 已省略大部分（`seg_count` 为实际段数）。

## 6. 端到端示例

**Mosquitto 命令行（控制 + 监听）：**

```bash
# 终端 1：监听状态与红外帧
mosquitto_sub -h 192.168.1.100 -t 'ir-web/status' -t 'ir-web/frame' -v

# 终端 2：订阅响应主题，然后发命令
mosquitto_sub -h 192.168.1.100 -t 'ir-web/rsp' -v &
mosquitto_pub -h 192.168.1.100 -t 'ir-web/cmd' -m '{"cmd":"status"}'
mosquitto_pub -h 192.168.1.100 -t 'ir-web/cmd' \
  -m '{"id":"a2","cmd":"play","body":{"type":"hxd","value":"ED127F80"}}'
```

**Python（paho-mqtt）监听红外帧：**

```python
import json
import paho.mqtt.client as mqtt

def on_message(client, userdata, msg):
    d = json.loads(msg.payload)
    if msg.topic.endswith("/frame"):
        nec = d.get("nec", {})
        print(f"frame #{d['seq']} hxd={nec.get('hxd')} segs={d['feat']['seg_count']}")

c = mqtt.Client()
c.on_message = on_message
c.connect("192.168.1.100")
c.subscribe("ir-web/status")
c.subscribe("ir-web/frame")
c.loop_forever()
```

**常见用途：**

- **控制**：向 `ir-web/cmd` 发 `play/carrier/rxpause` 等命令，从 `ir-web/rsp` 收结果；
- **监视**：订阅 `ir-web/frame` 实时获取每个红外信号，接入 Home Assistant / Node-RED；
- **在线状态**：订阅 `ir-web/status`，收到 `offline` 即设备掉线（LWT）。

配套的完整远程遥控前端（Vue SPA + Cloudflare Worker + 云端码库）见
[Web-app 子项目](Web-app子项目.md)。

## 7. 注意事项与限制

- 仅在 **STA 连接路由器**时启动 MQTT；纯 SoftAP 模式不连接；
- **多设备部署**：同一 Broker 下多台设备务必保持默认 Client ID（自动按 MAC 生成）
  或每台设置不同 ID，并开启"主题自动带设备标识"；否则主题互相覆盖、命令会被所有
  设备同时执行；
- 红外帧固定 QoS 0（实时尽力而为，同 WebSocket 推送语义），命令/状态按配置 QoS 发送；
- 帧 JSON 可达数 KB，MQTT 收发缓冲已设为 12288 字节（`buffer.size`），整帧单包发送；
  若某帧 JSON 超过 12288 字节则无法单包发送，将被丢弃并在响应主题发布错误响应；
  **命令负载同样受该上限约束**——超过上限的命令会被忽略，设备在响应主题回发
  `{"ok":false,"error":"command too large"}`。帧大小与 `IR_TOOL_MAX_RX_SEGS` 成正比，
  如需传输更大帧请同步增大 `buffer.size`；
- **协议版本**：Broker 只支持 3.1.1 而设备配置为 5.0（或反之）时，客户端会持续重连失败，
  需在设置页把协议改为与 Broker 匹配的版本后重启；
- **安全**：MQTT 命令通道不经过 Web 登录认证（无 WebSocket 的 token 机制），安全性依赖
  Broker 的账号密码/TLS；请勿在公网匿名 Broker 上暴露命令主题。出于安全考虑，MQTT 通道
  **禁用账号/网络配置与会话类命令**（`authcfg` / `wificfg` / `webcfg` / `mqttcfg` / `wsorigin` / `logout` / `renew`，
  回复 `error:"command not allowed on MQTT"`），仅可执行读取与操作类命令
  （`status` / `play` / `carrier` / `rxpause` / `frames`）。
