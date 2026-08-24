# esp32c3-ir-web — ESP32-C3 红外信号 Web 监视器（NEC 解码 / 回放）

基于 **ESP32-C3 SuperMini（4MB Flash，ESP32C3FN4）** 与 **ESP-IDF v6.0.2**。
通过 VS1838B 接收红外遥控信号，RMT 以 2us 分辨率采集原始波形，NEC 解码后在 **Web 页面**实时显示，
并支持 **NEC hxd**（32 位 LSB 十六进制）与 **原始数据**（Frequency + 微秒序列）回放。

## 功能一览

- **实时监视**：Web 页面通过 WebSocket 实时显示最新红外信号（NEC 解码、hxd、特征、原始波形数据），无需轮询
- **NEC 解码**：支持 8/16 位地址、重复码、校验和检测（极性无关扫描 9ms 引导码）
- **原始数据**：按 `Frequency: 38000 Hz` + 逗号分隔微秒序列格式显示，可直接复制回放
- **三种回放**：NEC hxd（如 `ED127F80`，LSB 顺序）/ 原始数据（可带 `Frequency:` 行）/ RAM 历史帧
- **载波 Web 可配置**：默认 38000 Hz，Web 设置即时生效并写入 NVS（重启保留）
- **回放暂停接收**：回放时可自动暂停 IR 接收，避免自环帧干扰，开关持久化到 NVS
- **WiFi 自动互斥**：配置了连接 WiFi 则只连路由器（不开热点）；未配置则只开热点（SoftAP）；STA 连接超时自动降级 SoftAP，保证 Web 始终可达
- **Web 登录认证**：默认 admin / admin，WebSocket 登录 + session token 管理，可在设置页修改账号密码、开关单设备登录
- **前后端分离（可选）**：可关闭内置页面仅保留 `/api/ws`，供外部前端连接
- **MQTT 接入（可选）**：与 WebSocket 完全相同的命令 RPC + 状态/红外帧推送，可接入 Home Assistant / Node-RED 等

## 文档导航

| 文档 | 内容 |
|------|------|
| [快速开始](docs/01-快速开始.md) | 硬件连接、编译烧录、menuconfig 配置项、恢复出厂、NVS 加密 |
| [使用指南](docs/02-使用指南.md) | WiFi 模式、基本使用、设置页面、登录认证与安全 |
| [WebSocket API](docs/03-WebSocket-API.md) | `/api/ws` 全协议：登录/RPC 命令/推送消息/保活约定 |
| [MQTT 接入](docs/04-MQTT-接入.md) | 启用配置、主题、命令协议、状态/帧推送、端到端示例 |
| [部署指南](docs/05-部署指南.md) | 前后端分离、Cloudflare Pages/Tunnel、nginx 反代 |
| [工程结构与实现要点](docs/06-工程结构与实现要点.md) | 目录结构、数据流、sdkconfig 关键项、实现细节 |
| [Web-app 子项目](docs/Web-app子项目.md) | Vue 3 远程遥控前端：云端码库、学习模式、Cloudflare 部署 |

## 最小上手路径

```powershell
& 'D:\esp\v6.0.2\esp-idf\export.ps1'
cd G:\esp32s3\esp32c3-ir-web-ESP32-C3
idf.py set-target esp32c3
idf.py build
idf.py -p <PORT> flash monitor   # 例如 -p COM7
```

上电后设备开启热点 `ESP32C3-IR`（无密码），浏览器打开 `http://192.168.4.1`，
默认账号 **admin / admin**（首次登录强制改密）。详见[快速开始](docs/01-快速开始.md)。

## 项目地址与开源许可

- **项目地址**：<https://github.com/wty2019wty/esp32c3-ir-web-ESP32-C3/>
- **开源许可证**：[GPL-3.0](LICENSE)（GNU General Public License v3.0）

本项目基于 GPL-3.0 许可证开源。你可以自由使用、修改和分发本项目的代码，
但基于本项目代码的任何衍生作品也必须以 GPL-3.0 协议开源，并保留原始版权声明。
详见根目录 [LICENSE](LICENSE) 文件。
