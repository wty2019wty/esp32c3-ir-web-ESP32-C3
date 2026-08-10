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
  防止暴力破解。
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
  - 连续失败 5 次后锁定 30 秒，防止暴力破解。
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
      （密码不回显，`ap_password_set`/`sta_password_set` 标志；密码传 `null` 表示不修改、空字符串表示清除）
    - `authcfg`：body 含 `user`/`pass`/`single_session` 任一字段 = 保存设置，body 为空 = 读取
      （`{"user":...,"single_session":bool}`）。保存返回 `{"invalidated":bool}`：是否
      "变更"由服务端与已保存值比较判定，**只有 `user`/`pass` 实际发生变化才作废会话、
      需要重新登录**（`invalidated:true`，前端据此提示重登）；
      重复提交相同值、或只切换 `single_session` 均返回 `invalidated:false` 且不会踢掉任何会话
    - `renew`：续期会话（`{"expires_in":N}`）
    - `logout`：退出登录，响应后服务端关闭连接
    - `webcfg`：body 含 `web_ui`（bool）= 设置"启用内置 Web 界面"开关并重启
      （`{"restart":true}`）；body 为空 = 读取（`{"web_ui":bool}`）
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
  递增会话代数，令所有旧会话立即失效。

`status` 推送（及 `status` 命令）中的 `data` 字段：`mode`、`ap_ip`、`sta_ip`、`ap_ssid`、
`sta_ssid`、`sta_ip_mode`、`sta_connected`、`carrier_hz`、`rx_pause_on_play`、`playing`。
单帧对象字段：`seq`、`ts`、`nec{...}`、`feat{...}`、`freq`、`durs[...]`。

### MQTT（可选，命令 RPC + 状态/帧推送）

设备内置 MQTT 客户端（`espressif/mqtt` 组件），复用与 WebSocket 完全相同的命令核心
与 JSON 序列化。**STA 连接路由器时才启动 MQTT**；纯 SoftAP（无外网/无到 Broker 路由）模式下
客户端保持停止。

**配置**：默认值来自 menuconfig（或改 `sdkconfig.defaults` 后重新编译），
也可以在 **Web 设置页 → MQTT 设置**直接修改（保存后 2 秒自动重启，写入 NVS 并优先于默认值）：

- `IR_TOOL_MQTT_ENABLE`：总开关（默认开）
- `IR_TOOL_MQTT_BROKER_URI`：Broker 地址，如 `mqtt://192.168.1.100:1883`；
  **留空 = 运行时禁用 MQTT**
- `IR_TOOL_MQTT_USERNAME` / `IR_TOOL_MQTT_PASSWORD`：Broker 认证（留空 = 匿名）
- `IR_TOOL_MQTT_CLIENT_ID`：客户端 ID（留空 = 按 MAC 自动生成 `ir-web-XXXXXX`）
- `MQTT 协议`：**Web 设置页可选 MQTT 3.1.1（默认）或 MQTT 5.0**，保存重启后生效；
  两种协议代码均编译进固件（`CONFIG_MQTT_PROTOCOL_5=y`），Broker 不支持 5.0 时切回 3.1.1 即可
- `MQTT over WebSocket`：Broker 地址填 `ws://host:port/path`（如 `ws://192.168.1.100:9001/mqtt`）
  即通过 WebSocket 传输连接，`wss://` 为 WebSocket + TLS；也可照常使用 `mqtt://`（TCP）、
  `mqtts://`（TLS）。MQTT 的 WebSocket 是**出站客户端连接**，与设备内置的
  `/api/ws` WebSocket **服务端**完全独立，互不影响
- `TLS 证书校验`：**默认使用内置证书包校验**（ESP-IDF 内置全量公共 CA，适合公共 Broker
  的 `mqtts://`/`wss://`）；自签名证书或内网 CA 的 Broker 请在 Web 设置页改为
  **跳过校验**（`CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y` 已启用，作为跳过时的回退）
- `IR_TOOL_MQTT_TOPIC_CMD/RSP/STATUS/FRAME`：主题，默认
  `ir-web/cmd`、`ir-web/rsp`、`ir-web/status`、`ir-web/frame`
- `IR_TOOL_MQTT_QOS`：QoS（默认 1）；`IR_TOOL_MQTT_PUBLISH_FRAMES`、`IR_TOOL_MQTT_PUBLISH_STATUS`
- 帧 JSON 可能达数 KB，`CONFIG_MQTT_BUFFER_SIZE` 已在 `sdkconfig.defaults` 中放大到 12288

**命令（发到 `ir-web/cmd`）**：JSON 信封与 WebSocket 命令一致
（`cmd`/`body` 字段完全相同），额外支持可选 `id` 关联请求与响应：

```json
{"id":"a1","cmd":"status"}
{"id":"a2","cmd":"play","body":{"type":"hxd","value":"ED127F80","freq":38000}}
{"id":"a3","cmd":"frames","body":{"since":0}}
```

也支持直接发送裸命令名（如 `status`）。响应发布到 `ir-web/rsp`：

```json
{"ok":true,"id":"a1","cmd":"status","result":{...}}
{"ok":false,"id":"x","cmd":"play","error":"playback failed"}
```

**推送**：

- `ir-web/status`：连接成功时发布完整状态对象（retained，便于新订阅者立即拿到），
  播放开始/结束时再次发布；断线时 Broker 按 LWT 发布 `offline`（retained，覆盖旧状态）
- `ir-web/frame`：每次捕获到新红外帧立即发布，单帧对象与 WebSocket 推送完全一致
  （含 `nec` 解码、`feat` 特征、`freq`、`durs` 原始波形）

**安全说明**：MQTT 命令通道**不经过 Web 登录认证**（没有 WebSocket 的 token 机制），
安全性依赖 Broker 的账号密码/TLS；请勿在公网匿名 Broker 上暴露命令主题。

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
esp32c3-IR/
├── CMakeLists.txt
├── sdkconfig.defaults        # esp32c3 / 4MB flash / 自定义分区 / NVS 加密 / WS 支持
├── partitions.csv            # nvs + phy_init + factory（约 3.9MB app 区）
├── api-demo.py               # Python 示例脚本：WS 登录 + 回放/监听（WebSocket-only）
├── .github/workflows/        # deploy-pages.yml：前端部署到 GitHub Pages
└── main/
    ├── CMakeLists.txt        # EMBED_TXTFILES 内嵌 index.html
    ├── idf_component.yml     # 依赖 espressif/cjson、espressif/mqtt
    ├── Kconfig.projbuild     # 引脚/载波/WiFi/HTTP/认证/MQTT 配置项
    ├── app_main.c            # NVS → IR → WiFi → Web → MQTT
    ├── app_ir.c              # RMT RX/TX 核心 + 载波 + RAM 历史
    ├── app_ir_nec.c          # NEC 协议解码
    ├── app_ir_play.c         # hxd/raw 回放编码
    ├── app_ir_store.c        # 帧存储 + 历史环形缓冲 + 回调
    ├── app_wifi.c            # AP/STA 互斥 + 超时降级
    ├── app_mqtt.c            # MQTT 客户端：命令 RPC + 状态/帧推送 + WiFi 联动
    ├── app_web.c             # HTTP server 初始化（仅静态页面 + WS 端点）
    ├── app_web_api_ir.c      # IR 命令核心（play/carrier/rxpause，WS-only）
    ├── app_web_api_wifi.c    # WiFi 配置核心（wificfg 读写，WS-only）
    ├── app_web_auth.c        # WS 登录 / token 认证 / 会话代数 / 凭据管理
    ├── app_web_rpc.c         # 命令分发（WS cmd → 各模块）+ 帧历史 JSON 构建
    ├── app_web_ws.c          # WebSocket /api/ws：登录认证、命令 RPC、推送（串行发送 + 心跳）
    ├── app_web_util.c        # JSON 序列化 + HTTP 工具函数
    ├── web/index.html        # 前端单页（内嵌，无需文件系统）
    ├── web/run.bat           # 本地静态服务器（python -m http.server 80），前后端分离调试用
    └── include/              # 各模块头文件
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
