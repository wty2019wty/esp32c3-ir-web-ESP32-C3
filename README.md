# esp32c3-ir-web — ESP32-C3 红外信号 Web 监视器（NEC 解码 / 回放）

基于 **ESP32-C3 SuperMini（4MB Flash，ESP32C3FN4）** 与 **ESP-IDF v6.0.2**。
通过 VS1838B 接收红外遥控信号，RMT 以 2us 分辨率采集原始波形，NEC 解码后在 **Web 页面**实时显示，
并支持 **NEC hxd**（32 位 LSB 十六进制）与 **原始数据**（Frequency + 微秒序列）回放。

## 功能

- **实时监视**：Web 页面实时显示最新红外信号（NEC 解码、hxd、特征、原始波形数据），
  通过 WebSocket 推送，无需轮询
- **NEC 解码**：支持 8/16 位地址、重复码、校验和检测（极性无关扫描 9ms 引导码）
- **原始数据**：按 `Frequency: 38000 Hz` + 逗号分隔微秒序列格式显示，可直接复制回放
- **三种回放**：
  - NEC hxd（如 `ED127F80`，LSB 顺序）
  - 原始数据（可带 `Frequency:` 行）
  - RAM 历史帧（点击历史记录回放）
- **载波 Web 可配置**：默认 38000 Hz，Web 设置即时生效并写入 NVS（重启保留）
- **回放暂停接收**：回放时可自动暂停 IR 接收，避免自环帧干扰，开关持久化到 NVS
- **WiFi 自动互斥**：配置了连接 WiFi 则只连路由器（不开热点）；未配置则只开热点（SoftAP）；
  STA 连接超时自动降级 SoftAP，保证 Web 始终可达
- **Web 登录认证**：默认 admin / admin，通过 WebSocket 登录并管理 session token，
  可在 Web 设置页修改账号密码
- **单设备登录（默认开启）**：开启后每次登录都会签发全新 token 并踢出所有旧会话，
  防止他人用相同账号在别处登录；可在 Web 设置页关闭（关闭后同账号多端共享会话）
- **前后端分离（可选）**：登录前可手动填写设备 ws(s) 地址；设置页的"启用内置 Web 界面"
  开关可让设备**不提供页面、仅保留 `/api/ws`**，供外部前端连接
- **MQTT 接入（可选）**：订阅命令主题即可执行与 WebSocket 完全相同的 RPC 命令，
  自动向主题推送实时状态与红外帧（NEC 解码 + 原始波形），可接入 Home Assistant / Node-RED 等；
  Broker 地址、账号、主题等可在 **Web 设置页**直接修改

## 硬件连接

|功能|GPIO|说明|
|----|----|----|
|IR 接收|GPIO4|VS1838B OUT（解调后基带信号，空闲为高电平）|
|IR 发射|GPIO3|需外接三极管驱动红外发光二极管|

所有引脚可在 `idf.py menuconfig` → "IR Web Tool Configuration" 中修改。

## 编译与烧录

在 PowerShell 中：

```powershell
& 'D:\esp\v6.0.2\esp-idf\export.ps1'
cd G:\esp32s3\esp32c3-ir-web-ESP32-C3
idf.py set-target esp32c3
idf.py build
idf.py -p <PORT> flash monitor   # 例如 -p COM7
```

> 首次构建时组件管理器自动下载 `espressif/cjson` 依赖（无需手动操作）。
> 首次烧录后 NVS 分区会自动初始化。

## 配置（menuconfig）

```powershell
idf.py menuconfig   # → "IR Web Tool Configuration"
```

|配置项|默认值|说明|
|------|------|----|
|IR receiver GPIO|4|VS1838B OUT|
|IR transmitter GPIO|3|三极管驱动 IR LED|
|Default IR carrier|38000 Hz|初始载波，Web 可改并持久化|
|Carrier duty cycle|33%|载波占空比|
|SoftAP SSID / password|ESP32C3-IR / 空|热点名称/密码（空 = 开放网络；可在 Web 设置页修改）|
|SoftAP channel|1|热点信道|
|SoftAP max connections|4|热点最大连接数|
|Station SSID / password|空|连接的路由器凭据（Web 设置页修改；非空则自动切 STA 模式）|
|STA timeout|10000 ms|STA 连接超时后降级为 SoftAP|
|Max STA TX power|15 dBm|STA 最大发射功率（部分 C3 板默认 20 dBm 开放认证失败，调低可稳定连接）|
|DHCP hostname|ir-web|路由器客户端列表中显示的主机名|
|HTTP port|80|Web 服务端口|
|History depth|8|RAM 历史帧条数（每帧约 4.2KB，8 帧 ≈ 34KB）|
|Max RX segments|1024|每帧最大交替段数（空调协议常超 300 段）|
|Max TX symbols|2048|原始数据回放的最大 RMT 符号数|

> 工作模式自动互斥：配置了 STA SSID 则设备只连接路由器（不开热点）；STA SSID 为空则只开热点。
> 两者不会同时开启。

## 使用

1. 上电后自动选择模式：
   - **配置了连接 WiFi**：设备连接路由器，IP 见串口日志，浏览器打开 `http://<IP>`（此时不开热点）
   - **未配置（默认）**：设备开启热点 `ESP32C3-IR`（无密码），浏览器打开 `http://192.168.4.1`
2. 将遥控器对准 VS1838B 按键，页面实时显示 NEC 解码与原始数据
3. 回放：点击"回放此信号"或历史记录行；也可粘贴 `ED127F80` 或
   `Frequency: 38000 Hz` + 逗号序列到"手动回放"区域
4. 需要不同载波时（如 36k/40k），在"载波频率"输入并点击"设置载波"

### 设置页面与恢复出厂

- 页面顶部切换到 **设置** 页，可配置：热点名称/密码、连接的路由器 WiFi、
  STA 地址获取方式（DHCP / 静态 IP、掩码、网关、DNS）、Web 登录账号密码、
  **单设备登录开关**（默认开启）。
  **服务模式**："启用内置 Web 界面"默认勾选；取消勾选后设备重启将**不再提供内置页面**，
  仅保留 `ws://<IP>/api/ws`（前后端分离场景，需用外部前端连接）。保存后设备 2 秒自动重启生效。
- **恢复出厂**：开机后 **2 秒内按住 BOOT 键**（GPIO9）约 50ms，
  设备会擦除全部配置（NVS）并重启，回到默认的**无密码热点**（SSID `ESP32C3-IR`、
  开放网络、DHCP、38kHz 载波、回放暂停接收开启、Web 页面开启）。
  注意：上电瞬间就按住 BOOT 会进入 ROM 下载模式（固件不运行），恢复出厂需在
  固件启动后的 2 秒窗口内按下。

### Web 登录认证

- 默认账号 **admin / admin**，存 NVS；**首次用默认凭据登录会强制修改密码**
  （WS 登录返回 `must_change_pwd`，前端弹出强制改密对话框，保存后自动重新登录）。
- 访问页面时浏览器自动连接 `ws://<IP>/api/ws` 并弹出登录框，输入用户名密码后
  通过 **WS 登录**获得 session token（**该连接随即成为已认证会话**）；token 有效期为 **24 小时**，
  前端会自动续期（`renew` 命令），过期后需重新登录。
- 登录连续失败 **5 次** 后锁定 **30 秒**（`login` 回复 `error:"too many attempts","retry_after":N`），
  防止暴力破解。锁定按**来源 IP** 分别计数，局域网内一个客户端恶意输错密码不会把
  其他客户端（含管理员）锁在门外。
- 支持退出登录（`logout` 命令），服务端立即作废当前 token 并关闭该连接。
- **单设备登录**：设置页"Web 登录"卡片提供**单设备登录**开关（**默认开启**）。开启时，
  只要设备上已有活跃会话，任何一次新登录都会**生成全新 token 并递增会话代数**，
  此前所有已认证的 WebSocket 连接随即被踢下线（命令被拒、推送停止），从而防止
  多个设备/浏览器同时在线；关闭后回到原来的行为：同账号多端共享同一 token、
  互不踢下线（便于多标签页同时使用）。
- WiFi 密码（热点/路由器）**不再回显明文**，设置页只显示"已设置"，留空表示不修改，
  勾选"清除"才会删除；`wificfg` 读取仅返回 `ap_password_set` / `sta_password_set` 标志。
- 修改登录凭据后会强制注销，需用新账号重新登录（仅切换单设备登录开关不强制重新登录）。

### NVS 加密

- 固件启用 NVS 加密（`CONFIG_NVS_ENCRYPTION`），采用 **HMAC 方案**：NVS 加解密密钥由
  eFuse 中的 HMAC 密钥派生，**首次启动时自动生成并烧写一个 32 字节随机密钥到 eFuse
  `KEY4` 块**（`CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID=4`）。此烧写**不可逆**，烧写后该
  eFuse 块不可再用于其他用途。
- **升级注意**：现有设备此前 NVS 为明文存储，启用加密后旧数据无法解密，需要
  **擦除一次 flash**（如 `idf.py erase-flash`，或开机 2 秒内按 BOOT 恢复出厂），
  之后设备回到默认配置并触发强制改密流程。

### API（纯 WebSocket，无 REST）

**所有控制与数据全部走 `ws://<IP>/api/ws`**（REST API 已移除）。HTTP 服务器只提供
静态页面本身（`/`、`/index.html`）。唯一需要 token 引导的操作是 WS 登录。

### WebSocket（登录 + 命令 RPC + 推送）

页面加载后即连接 `ws://<IP>/api/ws`，所有 API（含登录）都在这一条连接上进行：

- **登录**（连接后第一条消息，唯一无需 token 的操作）：
  - 发送 `{"type":"login","user":"...","pass":"..."}`
  - 成功回复 `{"type":"login","ok":true,"token":"...","expires_in":86400,"must_change_pwd":bool}`
    —— **该连接随即成为已认证会话**，后续命令直接可用；
  - 失败回复 `{"type":"login","ok":false,"error":"bad credentials"}` 或
    `{"type":"login","ok":false,"error":"too many attempts","retry_after":N}`（连续失败 5 次锁定 30 秒）；
  - 连续失败 5 次后按来源 IP 锁定 30 秒，防止暴力破解。
- **已有会话**：客户端刷新页面时保存的 token 仍有效，连接后发送
  `{"type":"auth","token":"<token>"}` 认证（`auth` 消息与 `login` 二选一）；
  服务端回复 `{"type":"auth","ok":true}`；token 无效或过期则回复 `ok:false` 并断开。
- **命令 RPC**：
  - 客户端发送 `{"type":"cmd","id":N,"cmd":cmd,"body":{...}}`
  - 服务端回复 `{"type":"resp","id":N,"ok":true,"data":{...}}` 或
    `{"type":"resp","id":N,"ok":false,"error":"..."}`
  - `cmd` 取值（body 均省略 `ok` 包装）：
    - `play`：`{"type":"hxd","value":"ED127F80"}` / `{"type":"raw","data":[...],"freq":N}` / `{"type":"frame","seq":N}`，可选 `freq`
    - `carrier`：`{"freq":38000}`
    - `rxpause`：`{"enabled":true}`
    - `status`：body 为空，返回当前状态对象
    - `frames`：`{"since":N}`，增量拉取；超过约 48KB 时返回 `"truncated":true`，
      `last_seq` 为实际返回的最后一帧，客户端应使用该 `last_seq` 继续拉取直至追平
    - `wificfg`：body 含配置字段 = 保存并重启（`{"restart":true}`）；body 为空 = 读取
      （密码不回显，`ap_password_set`/`sta_password_set` 标志；密码传 `null` 表示不修改、空字符串表示清除）。
      校验：非法静态 IP / 超长 SSID / 1-7 位 STA 密码会被拒绝并返回具体错误（不再静默忽略）
    - `authcfg`：body 含 `user`/`pass`/`single_session` 任一字段 = 保存设置，body 为空 = 读取
      （`{"user":...,"single_session":bool}`）。保存返回 `{"invalidated":bool}`：是否
      "变更"由服务端与已保存值比较判定，**只有 `user`/`pass` 实际发生变化才作废会话、
      需要重新登录**（`invalidated:true`，前端据此提示重登）；
      重复提交相同值、或只切换 `single_session` 均返回 `invalidated:false` 且不会踢掉任何会话
    - `renew`：续期会话（`{"expires_in":N}`）
    - `logout`：退出登录，响应后服务端关闭连接
    - `webcfg`：body 含 `web_ui`（bool）= 设置"启用内置 Web 界面"开关并重启
      （`{"restart":true}`）；body 为空 = 读取（`{"web_ui":bool}`）
    - `wsorigin`：body 含 `origin`（字符串或 `null`）= 设置 WebSocket Origin 白名单
      （空 = 允许任意来源，默认）；body 为空 = 读取（`{"origin":string}`）。设置页"WebSocket 安全"卡片可配置。
- **推送消息**：
  - `{"type":"status","id":N,"data":{...}}` —— **状态有变化时才推送**（字段见下），
    携带递增 `id`；客户端收到后需回复 `{"type":"ack","id":N}` 确认抄收，
    未确认的客户端会每秒补发，直到确认或状态再次变化；
    **播放开始/结束时立即推送**（不依赖每秒采样，避免短暂的"播放中"状态被漏掉）
  - `{"type":"frame","data":{...}}` —— 收到新红外信号时立即推送（单帧对象，同 `frames` 中元素）
- 前端在认证成功后主动请求一次 `status` 与 `frames`（多段拉取直至追平，最多重试 8 次）
  完成初始同步；连接断开时前端自动重连（10 秒间隔）并重新登录/认证。
- **连接保活**：即使状态无变化，服务端也**每 20 秒**推送一次 status 心跳，
  让空闲连接穿过家用路由器/AP 的 NAT 会话回收（此前常表现为 `104 ECONNRESET` 掉线）；
  前端另有**假死看门狗**：45 秒内未收到任何服务器消息（心跳/推送/响应）即主动重连，
  并**每 15 秒**主动拉取一次 `status` 校验连接存活、刷新界面（被动推送无法区分
  "连接静默死亡"与"设备确实无变化"）。
- **并发安全与背压**：所有服务端→客户端帧都在 httpd 任务内**串行发送**（经
  `httpd_ws_send_data_async` 入队），避免多任务并发写同一 socket 造成字节交错、
  客户端解析出 "Invalid frame header"。每连接发送队列有上限（约 4 帧），
  高频回放 / 连续 IR 事件导致积压时丢弃多余帧，客户端可再用 `frames` 命令补齐。
- **鉴权与失效**：命令与推送均要求已认证会话；退出登录或修改登录凭据会使会话
  **代数**递增，已连接的 WebSocket 会话随即失效（命令被拒、推送停止），
  防止退出登录后残留连接仍可操作设备。开启**单设备登录**（默认）时，新登录同样
  递增会话代数，令所有旧会话立即失效。token 过期（24 小时）时服务端作废会话并
  递增代数——即使连接一直保持，过期后发起的命令也会被拒，需重新登录。
- **来源校验**：`/api/ws` 支持可选的 **Origin 白名单**（设置页"WebSocket 安全"或
  `wsorigin` 命令，默认空 = 允许任意来源）。填写后仅匹配的页面来源能建立连接，
  可阻止恶意网页发起跨站 WebSocket 连接；前后端分离场景请把托管页面所在的来源
  加入白名单。

`status` 推送（及 `status` 命令）中的 `data` 字段：`mode`、`ap_ip`、`sta_ip`、`ap_ssid`、
`sta_ssid`、`sta_ip_mode`、`sta_connected`、`carrier_hz`、`rx_pause_on_play`、`playing`。
单帧对象字段：`seq`、`ts`、`nec{...}`、`feat{...}`、`freq`、`durs[...]`。

### MQTT（可选，命令 RPC + 状态/帧推送）

设备内置 MQTT 客户端（`espressif/mqtt` 组件），复用与 WebSocket 完全相同的命令核心
与 JSON 序列化。**STA 连接路由器时才启动 MQTT**；纯 SoftAP（无外网/无到 Broker 路由）模式下
客户端保持停止。

#### 1. 启用与配置

默认值来自 menuconfig（或改 `sdkconfig.defaults` 后重新编译），也可以在
**Web 设置页 → MQTT 设置**直接修改（保存后 2 秒自动重启，写入 NVS 并优先于默认值）：

| 配置项 | 默认值 | 说明 |
|---|---|---|
| 启用 MQTT | 关 | `IR_TOOL_MQTT_ENABLE` |
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

#### 2. 主题一览（哪些订阅、哪些发送）

主题全部可在 Web 设置页修改，默认值如下。方向以**设备**为参考：

| 主题 | 默认值 | 设备方向 | 用途 |
|---|---|---|---|
| 命令主题 | `ir-web/cmd` | **订阅（收）** | 接收外部命令（JSON 信封） |
| 响应主题 | `ir-web/rsp` | **发送（发）** | 回传命令执行结果 |
| 状态主题 | `ir-web/status` | **发送（发）** | 发布状态 JSON；同时作为 LWT 离线主题 |
| 红外帧主题 | `ir-web/frame` | **发送（发）** | 每捕获一帧红外信号发布一帧 JSON |

即：设备**订阅 1 个主题**（命令），**发布 3 个主题**（响应/状态/帧）。外部客户端则相反：
要控制设备就**向 `ir-web/cmd` 发布**并**订阅 `ir-web/rsp`** 收响应；要监视就
**订阅 `ir-web/status` 和 `ir-web/frame`**。

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

#### 3. 命令协议（控制设备）

命令 JSON 信封与 WebSocket 完全一致（`cmd` / `body` 字段相同），额外支持可选 `id`
字段用于关联请求与响应：

```json
{"id":"a1","cmd":"status"}
{"id":"a2","cmd":"play","body":{"type":"hxd","value":"ED127F80","freq":38000}}
{"id":"a3","cmd":"frames","body":{"since":0}}
```

也支持直接发送裸命令名（如 `status`）。

**`fpub` 用法示例（运行时开关帧推送，无需重启）：**

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

**支持的 `cmd`（发到 `ir-web/cmd`）：**

| cmd | body | 说明 |
|---|---|---|
| `status` | 空 | 返回当前状态对象 |
| `play` | `{"type":"hxd","value":"ED127F80"}` / `{"type":"raw","data":[...],"freq":N}` / `{"type":"frame","seq":N}`，可选 `freq` | 回放：NEC hxd / 原始数据 / 历史帧 |
| `carrier` | `{"freq":38000}` | 设置载波频率并持久化 |
| `rxpause` | `{"enabled":true}` | 回放时是否暂停接收 |
| `frames` | `{"since":N}` | 增量拉取帧历史；超 48KB 返回 `"truncated":true`，按 `last_seq` 继续拉取 |
| `fpub` | `{"enabled":true}` | **运行时**开关红外帧推送（不写 NVS，重启恢复保存的「推送帧」配置）；缺省 `enabled` 时仅返回当前状态 `{"publish_frames":true}` |
| ~~`renew`~~ | 空 | **MQTT 通道禁用**（续期 Web 会话属会话敏感操作，MQTT 无会话故无用；仅 WebSocket 通道可执行） |
| ~~`wificfg`~~ / ~~`authcfg`~~ / ~~`webcfg`~~ / ~~`mqttcfg`~~ / ~~`wsorigin`~~ / ~~`logout`~~ | — | **MQTT 通道禁用**（配置/凭据/会话敏感命令，回复 `command not allowed on MQTT`；仅 WebSocket 通道可执行） |

**响应格式（发布到 `ir-web/rsp`）：**

```json
{"ok":true,"id":"a1","cmd":"status","result":{...}}
{"ok":false,"id":"x","cmd":"play","error":"playback failed"}
```

- `ok`：执行结果；`id`/`cmd` 与请求对应，便于多命令并发时关联；
- `result`：成功时的返回数据（JSON 对象，即 WebSocket 响应的 `data`）；
- `error`：失败时的简短错误信息。

#### 4. 状态推送（`ir-web/status`）

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

字段含义：`mode`（AP/STA）、`ap_ip`/`sta_ip`（IP，未用为空串）、`sta_ip_mode`
（dhcp/static/-）、`carrier_hz`（当前载波）、`rx_pause_on_play`（回放暂停接收开关）、
`playing`（是否正在回放）。

#### 5. 红外帧推送（`ir-web/frame`）

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

字段：`seq`（递增序号）、`ts`（uptime 毫秒）、`nec`（NEC 解码结果：`ok`/`repeat`/
`ext` 16 位地址/`chksum`/`bits`/`addr`/`cmd`/`raw`/`hxd` LSB 十六进制）、`feat`
（波形特征：总时长、脉冲数、最小/最大脉冲、引导码、尾间隙、段数）、`freq`
（**采集瞬间**的载波频率，之后修改全局载波不会改写历史帧的标签）、
`durs`（交替电平微秒序列，首段为载波开）。NEC 解码失败时 `nec.ok=false`。
示例中 `durs` 已省略大部分（`seg_count` 为实际段数）。

#### 6. 端到端示例

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

#### 7. 注意事项与限制

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

### 前后端分离

页面可与设备分离部署（如托管在 CF Pages / 任意静态服务器）。页面内置的
Content-Security-Policy 已放行任意 `ws:`/`wss:` 源（`connect-src 'self' ws: wss:`），
WebSocket 不受 CORS 限制，因此从任意域名加载页面都能连接设备。此时：

- 页面加载后只会连接 **localStorage 中保存的设备地址**（无保存值时按当前页面域名，
  对分离部署而言通常是无效地址、连接失败后每 10 秒重试）；因此首次使用时
  **登录框内需先手动填写设备地址**（支持
  `ws://192.168.0.145:80`、`wss://ir.example.com` 或省略协议只填 `host:port`，
  HTTPS 页面下省略协议默认 `wss://`）；地址保存在浏览器 localStorage，下次自动填入并自动连接。
- 若设备关闭了"启用内置 Web 界面"（`webcfg` 的 `web_ui=false`），`/` 与 `/index.html` 返回 404，
  外部前端仍可通过 `/api/ws` 完整控制（登录、命令、推送）。
- **HTTPS 页面必须用 `wss://`**：HTTPS（如 CF Pages 的 `*.pages.dev`）页面里连接明文
  `ws://` 会被浏览器按**混合内容**拦截（`Mixed Content`），这是连接失败最常见的原因。
  设备自身只提供明文 `ws://`，需要借助反向代理/隧道把它变成 `wss://` 端点，见下。

#### 部署到 Cloudflare Pages

把 `main/web/index.html` 直接推到 CF Pages（或任意静态托管）即可作为前端：

1. 前端页面部署到 `https://<your-pages>.pages.dev`（自动 HTTPS）。
2. 给设备套 **Cloudflare Tunnel**，把设备的 `/api/ws` 暴露成 `wss://ir.example.com/api/ws`
   （Tunnel 自动带 HTTPS，无需 VPS/证书）：
   - 在局域网一台常开主机（树莓派/NAS）安装并登录 `cloudflared`：

     ```bash
     cloudflared tunnel create ir-web
     cloudflared tunnel route dns ir-web ir.example.com
     ```

   - 配置文件 `~/.cloudflared/config.yml`：

     ```yaml
     tunnel: ir-web
     credentials-file: /root/.cloudflared/<tunnel-id>.json

     ingress:
       - hostname: ir.example.com
         service: http://192.168.0.145:80
       - service: http_status:404
     ```

   - 运行 `cloudflared tunnel run ir-web`。注意：Tunnel 到设备这一段是局域网明文 HTTP，
     登录凭据/token 会在 `ir.example.com` 与设备之间以明文传输，建议只允许 cloudflared
     主机访问设备（同一可信网段），并在 Cloudflare Access 上给该域名加访问策略。
3. 浏览器打开 `https://<your-pages>.pages.dev`，登录框填
   `wss://ir.example.com` + 账号密码即可远程控制（也可在设备设置页关闭内置页面）。

> 也可用 nginx/VPS 反向代理产生 `wss://`（见下节），原理相同。

### HTTPS 反向代理（nginx）部署注意

如果通过 nginx 反代给设备套 HTTPS（浏览器 → HTTPS → nginx → 明文 HTTP → ESP32），注意以下几点：

- **信任边界**：`nginx → 设备` 这一段是明文 HTTP。所有控制都走 WebSocket，
  登录凭据与 session token 在 WS 帧内明文传输。能嗅探内网的人可以截获登录密码/token
  （有效期内等于完整管理员权限）。建议把 nginx 与设备放在同一可信网段
  （如专用 VLAN、仅允许 nginx 访问设备的 80 端口），或在设备自身启用 HTTPS
  （`esp_https_server` + mbedTLS）以消除明文段。
- **nginx 日志**：默认 `access_log` 不记录请求体，登录密码/token 不会落盘；
  不要自定义 `log_format` 打印请求体。
- **强制 HTTPS**：配置 HTTP → HTTPS 跳转，避免用户用 `http://` 直接访问导致凭据走明文。
- **WebSocket 必须走 wss**：外部前端（HTTPS 页面）需连接 `wss://<host>/api/ws`，反代需要转发 Upgrade 头，
  示例：

```nginx
location /api/ws {
    proxy_pass http://192.168.0.145:80;
    proxy_http_version 1.1;
    proxy_set_header Upgrade $http_upgrade;
    proxy_set_header Connection "upgrade";
    proxy_set_header Host $host;
}
```

HTTPS 页面下浏览器会使用 `wss://`；若反代未转发 Upgrade 头，WebSocket 会连接失败，
**整个页面不可用**（无 REST 回退，登录与控制全部依赖 WS）。

## 工程结构

```text
esp32c3-ir-web-ESP32-C3/
├── CMakeLists.txt
├── sdkconfig.defaults        # esp32c3 / 4MB flash / 自定义分区 / NVS 加密 / WS 支持 / MQTT（协议+WS 传输+TLS）
├── partitions.csv            # nvs + phy_init + factory（约 3.9MB app 区）
├── dependencies.lock         # 组件依赖锁定（espressif/cjson、espressif/mqtt、idf 版本）
├── api-demo.py               # Python 示例脚本：WS 登录 + 回放/监听（WebSocket-only）
├── LICENSE
├── .github/workflows/        # build.yml：固件编译 CI；deploy-pages.yml：前端部署到 GitHub Pages
└── main/
    ├── CMakeLists.txt        # EMBED_TXTFILES 内嵌 index.html
    ├── idf_component.yml     # 依赖 espressif/cjson、espressif/mqtt
    ├── Kconfig.projbuild     # 引脚/载波/WiFi/HTTP/认证/MQTT 配置项
    ├── app_main.c            # NVS → IR → WiFi → Web → MQTT
    ├── app_ir.c              # RMT RX/TX 核心 + 载波 + RAM 历史
    ├── app_ir_nec.c          # NEC 协议解码
    ├── app_ir_play.c         # hxd/raw 回放编码
    ├── app_ir_store.c        # 帧存储 + 历史环形缓冲 + 多监听器回调（WS/MQTT 共用）
    ├── app_wifi.c            # AP/STA 互斥 + 超时降级
    ├── app_mqtt.c            # MQTT 客户端：命令 RPC + 状态/帧推送 + WiFi 联动
    ├── app_web.c             # HTTP server 初始化（仅静态页面 + WS 端点）
    ├── app_web_api_ir.c      # IR 命令核心（play/carrier/rxpause，WS/MQTT 共用）
    ├── app_web_api_wifi.c    # WiFi 配置核心（wificfg 读写，WS/MQTT 共用）
    ├── app_web_auth.c        # WS 登录 / token 认证 / 会话代数 / 凭据管理
    ├── app_web_rpc.c         # 命令分发（WS/MQTT cmd → 各模块）+ 帧历史 JSON 构建
    ├── app_web_ws.c          # WebSocket /api/ws：登录认证、命令 RPC、推送（串行发送 + 心跳）
    ├── app_web_util.c        # JSON 序列化 + HTTP 工具函数
    ├── web/index.html        # 前端单页（内嵌，无需文件系统）
    ├── web/run.bat           # 本地静态服务器（python -m http.server 80），前后端分离调试用
    └── include/              # 各模块头文件（app_ir.h / app_wifi.h / app_web.h / app_mqtt.h 等）
```

## 实现要点

- **C3 无 RMT DMA**：RX/TX 各用 96 symbols（2×48 内存块），驱动自动 ping-pong；
  单帧上限 96 symbols（NEC 完整帧 34 symbols），长原始数据回放由 copy encoder 分段发送；
  RX 用户缓冲区按 `Max RX segments`（默认 1024）分配，支持超长帧（如空调协议）
- **回放极性**：采集自 VS1838B（active-low）的帧在分析时归一化为"首段载波开"的
  逻辑序列，回放/显示直接复用；RMT TX 只在电平 1 时输出载波
- **载波动态切换**：`rmt_apply_carrier` 无状态限制，回放前可即时重设
- **内存**：历史帧静态环形缓冲（8 帧 × 约 4.2KB ≈ 34KB），不依赖文件系统；
  `ir_task` 栈大小为 `sizeof(ir_frame_t) + 4096`（随 `IR_RAW_MAX_SEGS` 动态调整）
- **RX 缓冲区溢出保护**：ISR 通过 `s_rx_overflow` 标志追踪部分接收事件，
  任务处理前检查符号数量阈值 `num > (IR_RAW_MAX_SEGS + 1) / 2`，
  超出则丢弃帧以避免 `ir_analyze` 截断导致解码错误；
  回放结束恢复 RX 时清除溢出标志
- **Web 数据流**：前端通过单条 WebSocket（`/api/ws`）完成登录、认证、命令与推送，
  **REST API 已移除**。服务器所有帧经 httpd 任务串行发送（入队异步发送 + 每连接队列上限），
  空闲时每 20 秒推送 status 心跳维持连接，前端 45 秒无数据即重连；
   命令与推送均要求已认证会话，登出/改密会立即使已连接会话失效。

## 项目地址与开源许可

- **项目地址**：<https://github.com/wty2019wty/esp32c3-ir-web-ESP32-C3/>
- **开源许可证**：[GPL-3.0](LICENSE)（GNU General Public License v3.0）

本项目基于 GPL-3.0 许可证开源。你可以自由使用、修改和分发本项目的代码，
但基于本项目代码的任何衍生作品也必须以 GPL-3.0 协议开源，并保留原始版权声明。
详见根目录 [LICENSE](LICENSE) 文件。
