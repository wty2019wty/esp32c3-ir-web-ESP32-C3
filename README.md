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
- **WiFi 双模式**：SoftAP（设备释放热点）/ Station（连接路由器）/ AP+STA；
  Station 连接超时自动降级 SoftAP，保证 Web 始终可达

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
| WiFi mode | AP | AP / STA / AP+STA 三选一 |
| SoftAP SSID / password | ESP32C3-IR / 空 | 空密码为开放网络（≥8 位时用 WPA2） |
| Station SSID / password | 空 | STA 模式凭据（空 = 跳过 STA） |
| STA timeout | 10000 ms | 连接超时后降级为 SoftAP |
| HTTP port | 80 | Web 服务端口 |
| History depth | 32 | RAM 历史帧条数 |

## 使用

1. 上电后按配置模式连接：
   - **AP 模式**：手机/电脑连接热点 `ESP32C3-IR`，浏览器打开 `http://192.168.4.1`
   - **STA 模式**：设备连接路由器，IP 见串口日志（`esp_wifi_connect` / `got IP` 日志），浏览器打开 `http://<IP>`
2. 将遥控器对准 VS1838B 按键，页面实时显示 NEC 解码与原始数据
3. 回放：点击"回放此信号"或历史记录行；也可粘贴 `ED127F80` 或
   `Frequency: 38000 Hz` + 逗号序列到"手动回放"区域
4. 需要不同载波时（如 36k/40k），在"载波频率"输入并点击"设置载波"

## 工程结构

```
esp32c3-IR/
├── CMakeLists.txt
├── sdkconfig.defaults        # esp32c3 / 4MB flash / 自定义分区
├── partitions.csv            # nvs + phy_init + factory（约 3.9MB app 区）
└── main/
    ├── CMakeLists.txt        # EMBED_TXTFILES 内嵌 index.html
    ├── idf_component.yml     # 依赖 espressif/cjson
    ├── Kconfig.projbuild     # 引脚/载波/WiFi/HTTP 配置项
    ├── app_main.c            # NVS → IR → WiFi → Web
    ├── app_ir.c              # RMT RX/TX + NEC 解码 + hxd/raw 回放 + 载波 + RAM 历史
    ├── app_wifi.c            # AP/STA/APSTA 三模式 + 超时降级
    ├── app_web.c             # HTTP server + JSON API
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
