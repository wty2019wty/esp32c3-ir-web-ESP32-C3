# esp32c3-ir-web — ESP32-C3 红外信号 Web 监视器（NEC 解码 / 回放）

基于 **ESP32-C3 SuperMini（4MB Flash，ESP32C3FN4）** 与 **ESP-IDF v6.0.2**。
通过 VS1838B 接收红外遥控信号，RMT 以 2us 分辨率采集原始波形，NEC 解码后在 **Web 页面**实时显示，
并支持 **NEC hxd**（32 位 LSB 十六进制）与 **原始数据**（Frequency + 微秒序列）回放。

## 功能

- **实时监视**：Web 页面实时显示最新红外信号（NEC 解码、hxd、特征、原始波形数据）
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
- **Web 登录认证**：默认 admin / admin，session token 认证，可在 Web 设置页修改账号密码

## 硬件连接

| 功能 | GPIO | 说明 |
|------|------|------|
| IR 接收 | GPIO4 | VS1838B OUT（解调后基带信号，空闲为高电平） |
| IR 发射 | GPIO3 | 需外接三极管驱动红外发光二极管 |

所有引脚可在 `idf.py menuconfig` → "IR Web Tool Configuration" 中修改。

## 编译与烧录

在 PowerShell 中：

```powershell
& 'D:\esp\v6.0.2\esp-idf\export.ps1'
cd G:\esp32s3\esp32c3-IR
idf.py set-target esp32c3
idf.py build
idf.py flash monitor
```

> 首次构建时组件管理器自动下载 `espressif/cjson` 依赖（无需手动操作）。
> 首次烧录后 NVS 分区会自动初始化。

## 配置（menuconfig）

```powershell
idf.py menuconfig   # → "IR Web Tool Configuration"
```

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| IR receiver GPIO | 4 | VS1838B OUT |
| IR transmitter GPIO | 3 | 三极管驱动 IR LED |
| Default IR carrier | 38000 Hz | 初始载波，Web 可改并持久化 |
| Carrier duty cycle | 33% | 载波占空比 |
| SoftAP SSID / password | ESP32C3-IR / 空 | 热点名称/密码（空 = 开放网络；可在 Web 设置页修改） |
| SoftAP channel | 1 | 热点信道 |
| SoftAP max connections | 4 | 热点最大连接数 |
| Station SSID / password | 空 | 连接的路由器凭据（Web 设置页修改；非空则自动切 STA 模式） |
| STA timeout | 10000 ms | STA 连接超时后降级为 SoftAP |
| DHCP hostname | esp32c3-ir-web | 路由器客户端列表中显示的主机名 |
| HTTP port | 80 | Web 服务端口 |
| History depth | 32 | RAM 历史帧条数 |
| Max RMT symbols | 2048 | 原始数据回放的最大 RMT 符号数 |

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
  STA 地址获取方式（DHCP / 静态 IP、掩码、网关、DNS）、Web 登录账号密码。保存后设备 2 秒自动重启生效。
- **恢复出厂**：开机后 **2 秒内按住 BOOT 键**（GPIO9）约 50ms，
  设备会擦除全部配置（NVS）并重启，回到默认的**无密码热点**（SSID `ESP32C3-IR`、
  开放网络、DHCP、38kHz 载波、回放暂停接收开启）。
  注意：上电瞬间就按住 BOOT 会进入 ROM 下载模式（固件不运行），恢复出厂需在
  固件启动后的 2 秒窗口内按下。

### Web 登录认证

- 默认账号 **admin / admin**，存 NVS，可在设置页修改。
- 访问 Web 页面时会弹出登录框，输入用户名密码后获得 session token，
  后续请求通过 `X-Auth-Token` 请求头携带 token 认证。
- 修改登录凭据后会强制注销，需用新账号重新登录。

### Web API

所有 API 需携带 `X-Auth-Token` 请求头（`/api/login` 除外）。

| 端点 | 方法 | 说明 |
|------|------|------|
| `/api/login` | POST | 登录认证，请求体 `{"user":"...","pass":"..."}` ，返回 `{"ok":true,"token":"..."}` |
| `/api/status` | GET | 设备状态（WiFi 模式、IP、载波、播放状态等） |
| `/api/frames` | GET | 增量拉取帧历史，参数 `?since=N`（N 为已知最大序号） |
| `/api/play` | POST | 回放信号，支持三种 type：`hxd`（`{"value":"ED127F80"}`）、`raw`（`{"data":[...]}`）、`frame`（`{"seq":N}`），可选 `freq` |
| `/api/carrier` | POST | 设置载波频率，`{"freq":38000}` |
| `/api/rxpause` | POST | 回放时暂停 IR 接收开关，`{"enabled":true/false}` |
| `/api/wificfg` | GET/POST | 读取/保存 WiFi 配置（热点名称、密码、路由器凭据、静态 IP 等） |
| `/api/authcfg` | GET/POST | 读取/修改登录凭据 |

## 工程结构

```
esp32c3-IR/
├── CMakeLists.txt
├── sdkconfig.defaults        # esp32c3 / 4MB flash / 自定义分区
├── partitions.csv            # nvs + phy_init + factory（约 3.9MB app 区）
└── main/
    ├── CMakeLists.txt        # EMBED_TXTFILES 内嵌 index.html
    ├── idf_component.yml     # 依赖 espressif/cjson
    ├── Kconfig.projbuild     # 引脚/载波/WiFi/HTTP/认证配置项
    ├── app_main.c            # NVS → IR → WiFi → Web
    ├── app_ir.c              # RMT RX/TX + NEC 解码 + hxd/raw 回放 + 载波 + RAM 历史
    ├── app_wifi.c            # AP/STA/APSTA 三模式 + 超时降级
    ├── app_web.c             # HTTP server + JSON API + 登录认证
    ├── web/index.html        # 前端单页（内嵌，无需文件系统）
    └── include/              # 各模块头文件
```

## 实现要点

- **C3 无 RMT DMA**：RX/TX 各用 96 symbols（2×48 内存块），驱动自动 ping-pong；
  单帧上限 96 symbols（NEC 完整帧 34 symbols），长原始数据回放由 copy encoder 分段发送
- **回放极性**：采集自 VS1838B（active-low）的帧在分析时归一化为"首段载波开"的
  逻辑序列，回放/显示直接复用；RMT TX 只在电平 1 时输出载波
- **载波动态切换**：`rmt_apply_carrier` 无状态限制，回放前可即时重设
- **内存**：历史帧静态环形缓冲（32 帧 × 约 1.1KB ≈ 36KB），不依赖文件系统
- **Web 数据流**：前端 500ms 轮询 `/api/frames?since=N` 增量拉取，服务端 chunked 输出
