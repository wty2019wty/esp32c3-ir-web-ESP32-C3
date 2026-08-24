# WebSocket API 参考

**所有控制与数据全部走 `ws://<IP>/api/ws`**（REST API 已移除）。HTTP 服务器只提供
静态页面本身（`/`、`/index.html`），唯一需要 token 引导的操作是 WS 登录。

页面加载后即连接 `ws://<IP>/api/ws`，所有 API（含登录）都在这一条连接上进行。
Python 客户端示例见根目录 `api-demo.py`。

## 登录 / 认证

### login（连接后第一条消息，唯一无需 token 的操作）

- 发送 `{"type":"login","user":"...","pass":"..."}`
- 成功回复 `{"type":"login","ok":true,"token":"...","expires_in":86400,"must_change_pwd":bool}`
  —— **该连接随即成为已认证会话**，后续命令直接可用；
- 失败回复 `{"type":"login","ok":false,"error":"bad credentials"}` 或
  `{"type":"login","ok":false,"error":"too many attempts","retry_after":N}`
  （连续失败 5 次按来源 IP 锁定 30 秒）。

### auth（已有会话时免登录）

客户端刷新页面时保存的 token 仍有效，连接后发送 `{"type":"auth","token":"<token>"}`
认证（`auth` 与 `login` 二选一）：

- 服务端回复 `{"type":"auth","ok":true}`；
- token 无效或过期则回复 `ok:false` 并断开。

## 命令 RPC

- 客户端发送 `{"type":"cmd","id":N,"cmd":cmd,"body":{...}}`
- 服务端回复 `{"type":"resp","id":N,"ok":true,"data":{...}}` 或
  `{"type":"resp","id":N,"ok":false,"error":"..."}`

### cmd 取值一览

| cmd | body | 说明 |
|-----|------|------|
| `play` | `{"type":"hxd","value":"ED127F80"}` / `{"type":"raw","data":[...],"freq":N}` / `{"type":"frame","seq":N}`，可选 `freq` | 回放：NEC hxd（LSB 顺序）/ 原始数据 / 历史帧 |
| `carrier` | `{"freq":38000}` | 设置载波频率并持久化到 NVS |
| `rxpause` | `{"enabled":true}` | 回放时是否暂停接收 |
| `status` | 空 | 返回当前状态对象 |
| `frames` | `{"since":N}` | 增量拉取帧历史；超过约 48KB 时返回 `"truncated":true`，`last_seq` 为实际返回的最后一帧，客户端应用该 `last_seq` 继续拉取直至追平 |
| `renew` | 空 | 续期会话（返回 `{"expires_in":N}`）|
| `logout` | 空 | 退出登录，响应后服务端关闭连接 |
| `wificfg` | body 含配置字段 = 保存并重启（`{"restart":true}`）；body 为空 = 读取 | WiFi 配置。密码不回显（只返回 `ap_password_set`/`sta_password_set` 标志；传 `null` 表示不修改、空字符串表示清除）。校验：非法静态 IP / 超长 SSID / 1-7 位 STA 密码会被拒绝并返回具体错误 |
| `authcfg` | body 含 `user`/`pass`/`single_session` 任一字段 = 保存；body 为空 = 读取（`{"user":...,"single_session":bool}`）| 登录配置。保存返回 `{"invalidated":bool}`：只有 `user`/`pass` 实际变化才作废会话需重登（`true`）；重复提交相同值或仅切换开关均返回 `false` 且不踢任何会话 |
| `webcfg` | body 含 `web_ui`（bool）= 设置"启用内置 Web 界面"开关并重启（`{"restart":true}`）；body 为空 = 读取（`{"web_ui":bool}`）| 服务模式开关 |
| `wsorigin` | body 含 `origin`（字符串或 `null`）= 设置 Origin 白名单（空 = 允许任意来源）；body 为空 = 读取（`{"origin":string}`）| WebSocket 安全 |

## 推送消息

### status（状态有变化才推送）

```json
{"type":"status","id":N,"data":{...}}
```

- 携带递增 `id`；客户端收到后需回复 `{"type":"ack","id":N}` 确认抄收，
  未确认的客户端会每秒补发，直到确认或状态再次变化；
- **播放开始/结束时立即推送**（不依赖每秒采样，避免短暂的"播放中"状态被漏掉）；
- 即使状态无变化，服务端也**每 20 秒**推送一次 status 心跳，
  让空闲连接穿过家用路由器/AP 的 NAT 会话回收（此前常表现为 `104 ECONNRESET` 掉线）。

`data` 字段：

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

字段含义：`mode`（AP/STA）、`ap_ip`/`sta_ip`（IP，未用为空串）、`sta_ip_mode`
（dhcp/static/-）、`sta_connected`、`carrier_hz`（当前载波）、`rx_pause_on_play`（回放暂停接收开关）、`playing`（是否正在回放）。

### frame（新红外信号即时推送）

```json
{"type":"frame","data":{...}}
```

单帧对象与 `frames` 命令中的元素一致：

| 字段 | 说明 |
|------|------|
| `seq` | 递增序号 |
| `ts` | uptime 毫秒 |
| `nec` | NEC 解码结果：`ok`/`repeat`/`ext`(16 位地址)/`chksum`/`bits`/`addr`/`cmd`/`raw`/`hxd`（32 位 LSB 十六进制）；解码失败时 `nec.ok=false` |
| `feat` | 波形特征：总时长、脉冲数、最小/最大脉冲、引导码、尾间隙、段数 |
| `freq` | **采集瞬间**的载波频率（之后修改全局载波不会改写历史帧的标签）|
| `durs` | 交替电平微秒序列，首段为载波开 |

## 客户端初始同步与保活约定

- 前端在认证成功后主动请求一次 `status` 与 `frames`（多段拉取直至追平，最多重试 8 次）
  完成初始同步；
- 连接断开时前端自动重连（10 秒间隔）并重新登录/认证；
- 前端另有**假死看门狗**：45 秒内未收到任何服务器消息（心跳/推送/响应）即主动重连，
  并**每 15 秒**主动拉取一次 `status` 校验连接存活、刷新界面（被动推送无法区分
  "连接静默死亡"与"设备确实无变化"）。

## 并发安全与鉴权语义

- **串行发送 + 背压**：所有服务端→客户端帧都在 httpd 任务内**串行发送**
  （经 `httpd_ws_send_data_async` 入队），避免多任务并发写同一 socket 造成字节交错、
  客户端解析出 "Invalid frame header"。每连接发送队列有上限（约 4 帧），
  高频回放 / 连续 IR 事件导致积压时丢弃多余帧，客户端可再用 `frames` 命令补齐；
- **鉴权与失效**：命令与推送均要求已认证会话；退出登录或修改登录凭据会使会话
  **代数**递增，已连接的 WebSocket 会话随即失效（命令被拒、推送停止），
  防止退出登录后残留连接仍可操作设备。开启**单设备登录**（默认）时，新登录同样
  递增会话代数，令所有旧会话立即失效。token 过期（24 小时）时服务端作废会话并
  递增代数——即使连接一直保持，过期后发起的命令也会被拒，需重新登录。
