# IR 万能遥控器（Web-app）

基于 [esp32c3-ir-web-ESP32-C3](https://github.com/wty2019wty/esp32c3-ir-web-ESP32-C3) 设备的**独立前端**：通过 **MQTT over WebSocket** 控制/监视设备，用 **Cloudflare Workers KV** 做云端红外码库（学习、存储、一键回放）。

技术栈：Vue 3 + Vite + mqtt.js，前端与 Worker 同域部署在 Cloudflare。

## 功能

- **遥控面板**：选择遥控器 → 按键网格一键回放（NEC hxd / 原始波形），发送记录实时反馈
- **学习模式**：捕获红外信号（监听帧主题推送 + 主动拉取设备 RAM 历史 `frames`），命名保存到云端码库
- **码库管理**：按遥控器分组浏览/删除，KV 持久化，跨设备共享
- **设备状态**：实时状态（模式/IP/载波/回放中）、载波设置、回放暂停接收开关
- **在线判定**：`status` 命令轮询 + 状态主题 + LWT 三路信号综合判断设备在线/离线
- **登录认证**：PBKDF2 哈希密码 + HMAC 签名 token，保护码库 API

## 架构

```
浏览器 (Vue SPA)
  │  HTTPS (CF 自动证书)
  ├── /api/*  ──►  Cloudflare Worker ──►  Workers KV（红外码库 + 账号）
  │                 └─ 登录认证（PBKDF2 密码哈希、HMAC-SHA256 token）
  └── ws/wss://  ──►  MQTT Broker（如 EMQX 8083 / Mosquitto 9001）
                        │ ▲
                        ▼ │
                   ESP32-C3 设备（esp32c3-ir-web 的 MQTT 客户端）
```

- 前端 ↔ Worker：HTTP（登录 + 码库 CRUD）
- 前端 ↔ 设备：MQTT over WebSocket，**必须连到与设备同一个 broker**（设备是 MQTT 客户端，不是 broker）
- 码库存 Cloudflare KV（`code:*`），账号存 KV（`auth:user` / `auth:pass`），token 签名密钥在 Worker secret

## 目录结构

```
Web-app/
├── package.json / vite.config.js / index.html
├── wrangler.toml          # KV 绑定 + [assets] 静态托管 + 初始账号 vars
├── worker/index.js        # Cloudflare Worker：登录认证 + 码库 CRUD API
├── src/
│   ├── main.js / App.vue  # 入口 + 三 Tab 布局 + 登录门控 + 设备在线轮询
│   ├── mqtt.js            # MQTT over WS 封装（命令 RPC / 状态 / 帧订阅）
│   ├── kv.js              # Worker API 客户端 + 登录态管理
│   ├── store.js           # 全局状态
│   ├── style.css
│   └── components/
│       ├── Login.vue          # 登录框
│       ├── ConnectPanel.vue   # broker/账号/主题配置（localStorage）
│       ├── DeviceStatus.vue   # 设备状态 + 载波/rxpause 设置
│       ├── LearnPanel.vue     # 学习模式（监听/拉取历史/保存入库/fpub 开关）
│       ├── CodeLibrary.vue    # 码库管理
│       └── RemotePad.vue      # 遥控面板（按键回放 + 发送记录）
```

## 与设备的 MQTT 对接

设备侧需：**STA 模式**连接路由器（纯 AP 热点模式下 MQTT 客户端不启动）、Web 设置页启用 MQTT 并填好 Broker 地址、协议版本与 broker 匹配。

默认主题（与设备一致，可在设置页修改）：

| 主题 | 前端方向 | 用途 |
|---|---|---|
| `ir-web/cmd` | 发布 | 命令 RPC（`status/play/carrier/rxpause/frames/fpub`） |
| `ir-web/rsp` | 订阅 | 命令响应（按 `id` 关联） |
| `ir-web/status` | 订阅 | 设备状态 + LWT `offline` |
| `ir-web/frame` | 订阅 | 红外帧推送 |

注意：
- 若设备开启"主题自动带设备标识"，实际主题变成 `ir-web/<client-id>/<cmd|rsp|status|frame>`，前端主题也要对应修改
- 命令信封 `{"id":"c1","cmd":"play","body":{...}}`，响应 `{"ok":true,"id":"c1","result":{...}}`
- MQTT 通道仅开放 `status/frames/play/carrier/rxpause/fpub`，配置/会话类命令被设备拒绝

## 本地开发

```powershell
cd Web-app
.\node_env.bat            # 激活项目内 Node v24（也可用系统 node）
npm install
npm run dev               # Vite dev server: http://localhost:5173
```

本地验证分工：
- **前端**：`npm run dev`（页面、MQTT 连接、UI）
- **KV API + 登录**：另开终端 `npx wrangler dev`（本地 workerd 模拟 KV），需要先设置本地 secret：
  ```powershell
  npx wrangler secret put AUTH_SECRET   # 本地开发也要，否则 /api/* 报错
  npx wrangler secret put ADMIN_PASS
  ```
  本地联调时前端默认请求同源 `/api`，需把 `src/kv.js` 的 `BASE` 改为 `http://localhost:8787/api`
- **MQTT**：本地起一个开了 WebSocket 端口（EMQX 8083 / Mosquitto 9001）的 broker，页面填 `ws://localhost:8083/mqtt`

## 构建与部署（Cloudflare）

```powershell
cd Web-app
.\node_env.bat
npm install

# 1. 创建 KV 命名空间，把返回的 id 填入 wrangler.toml
npx wrangler kv namespace create CODE_LIB

# 2. 设置 secrets（token 签名密钥 + 初始密码）
npx wrangler secret put AUTH_SECRET
npx wrangler secret put ADMIN_PASS

# 3. 构建前端并一次性部署（worker + 静态资源 + KV 绑定同域）
npm run build
npx wrangler deploy
```

`ADMIN_USER` 默认 `admin`（见 `wrangler.toml` 的 `[vars]`）。首次登录时 Worker 用 `ADMIN_USER`/`ADMIN_PASS` 初始化账号（PBKDF2 哈希入库），之后以 KV 记录为准。

## 登录与安全

- 密码：KV 只存 **PBKDF2-SHA256 哈希 + 随机盐**，不存明文
- Token：**HMAC-SHA256 无状态签名**，有效期 24 小时；密钥 `AUTH_SECRET` 通过 `wrangler secret` 注入
- 传输：CF 自动 HTTPS；token 存浏览器 localStorage
- 静态页面本体公开（浏览器需加载它才能显示登录框），**所有数据 API 均需 `Authorization: Bearer`**，token 无效/过期返回 401 并强制回登录页
- 登录失败统一回 `bad credentials`，不泄露账号是否存在

### 忘记密码

- **本地 dev**：删除本地 KV 认证记录后重启（`Remove-Item -Recurse .wrangler`），用当前 `ADMIN_PASS` 重新登录
- **生产**：`npx wrangler kv key delete --binding=CODE_LIB "auth:user"` 和 `"auth:pass"`，必要时先 `npx wrangler secret put ADMIN_PASS`；下一次登录请求会用新初始密码重新初始化账号（码库 `code:*` 不受影响）

## 常见问题

| 现象 | 排查 |
|---|---|
| 连不上 broker | 设备是否 STA 模式、MQTT 是否启用、broker WS 端口、HTTPS 页面必须 `wss://` |
| 学习模式无帧显示 | 用「拉取历史帧」（`frames` 命令）绕过推送帧/主题错配；或点「推送帧: 开」执行 `fpub` |
| 设备状态"已连接 · 探测中" | 等下一次 12s 轮询；确认 `status` 命令在 cmd 主题有响应 |
| 设备掉线但 badge 仍在线 | 正常断开不触发 LWT，靠 `status` 命令轮询最多 12s 判定离线 |
| 回放暂停接收开关无效 | 设备响应 `result.rx_pause_on_play` 会回写界面；确认命令走的是 MQTT 允许列表 |

## License

本项目为 [esp32c3-ir-web-ESP32-C3](https://github.com/wty2019wty/esp32c3-ir-web-ESP32-C3) 的配套前端，随主项目采用 GPL-3.0。
