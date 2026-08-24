# IR 万能遥控器（Web-app）

基于 [esp32c3-ir-web-ESP32-C3](https://github.com/wty2019wty/esp32c3-ir-web-ESP32-C3) 设备的**独立前端**：通过 **MQTT over WebSocket** 控制/监视设备，用 **Cloudflare Workers KV** 做云端红外码库（学习、存储、一键回放）。

技术栈：Vue 3 + Vite 7 + mqtt.js，前端与 Worker 同域部署在 Cloudflare。

## 功能

- **遥控面板**：选择遥控器 → 按键网格一键回放（NEC hxd / 原始波形），发送记录可折叠查看
- **学习模式**：捕获红外信号（监听帧主题推送 + 主动拉取设备 RAM 历史 `frames` 并按序号去重），会话内捕获列表可回看选中，命名保存到云端码库
- **码库管理**：按遥控器分组的卡片式列表（移动端友好，无横向表格），点卡片即回放，KV 持久化，跨设备共享
- **连接配置**：Broker 地址/账号/主题配置云端保存（密码 AES-GCM 加密存 KV），登录后自动回填并**自动尝试连接**；Broker 地址自动规范化——缺协议时按页面协议补 `ws://`/`wss://`，`mqtt(s)://` 自动转 WebSocket scheme，无路径自动补 `/mqtt` 端点
- **设备状态**：实时状态（模式/IP/载波/回放中）、载波设置、回放暂停接收开关
- **在线判定**：`status` 命令轮询 + 状态主题 + LWT 三路信号综合判断设备在线/离线
- **登录认证**：PBKDF2 哈希密码 + HMAC 签名 token（可吊销、登录限流），保护码库 API
- **界面体验**：移动端响应式布局、toast 操作反馈、空状态引导与学习提示

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
├── public/_headers        # 静态资源安全响应头（CSP / X-Frame-Options 等）
├── worker/index.js        # Cloudflare Worker：登录认证（限流/吊销）+ 码库 CRUD + MQTT 配置存取（AES-GCM）
├── src/
│   ├── main.js / App.vue  # 入口 + 三 Tab 布局 + 登录门控（退出先吊销服务端 token）+ 设备在线轮询
│   ├── mqtt.js            # MQTT over WS 封装（命令 RPC / 状态 / 帧订阅）
│   ├── kv.js              # Worker API 客户端 + 登录态管理
│   ├── store.js           # 全局状态
│   ├── style.css
│   └── components/
│       ├── Login.vue          # 登录框
│       ├── ConnectPanel.vue   # broker/账号/主题配置（云端 KV 加密存取 + 自动连接）
│       ├── DeviceStatus.vue   # 设备状态 + 载波/rxpause 设置
│       ├── LearnPanel.vue     # 学习模式（监听/拉取历史去重/捕获列表/保存入库/fpub 开关）
│       ├── CodeLibrary.vue    # 码库管理
│       └── RemotePad.vue      # 遥控面板（按键回放 + 发送记录）
```

## 与设备的 MQTT 对接

设备侧需：**STA 模式**连接路由器（纯 AP 热点模式下 MQTT 客户端不启动）、Web 设置页启用 MQTT 并填好 Broker 地址、协议版本与 broker 匹配。

默认主题（与设备一致，可在设置页修改）：

| 主题 | 前端方向 | 用途 |
|---|---|---|
| `ir-web/cmd` | 发布 | 命令 RPC（`status/play/carrier/rxpause/frames/fpub`） |
| `ir-web/rsp` | 订阅 | 命令响应（按 `id` 关联，id 带连接级随机前缀防多客户端串扰） |
| `ir-web/status` | 订阅 | 设备状态 + LWT `offline` |
| `ir-web/frame` | 订阅 | 红外帧推送 |

注意：
- 若设备开启"主题自动带设备标识"，实际主题变成 `ir-web/<client-id>/<cmd|rsp|status|frame>`，前端主题也要对应修改
- 命令信封 `{"id":"c1","cmd":"play","body":{...}}`，响应 `{"ok":true,"id":"c1","result":{...}}`
- Broker 地址只需填 host（或任意常见写法），前端会自动补全协议与 `/mqtt` 路径；HTTPS 页面下填明文 `ws://` 会被直接拦截提示
- 登录后若云端已保存完整配置（地址 + 账号密码），页面会**自动尝试连接**；连接失败不打扰，可到设置页手动排查
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

> **注意**：Cloudflare Workers 的 PBKDF2 迭代次数上限为 **100000**，代码中 `PBKDF2_ITER` 已设为该值。若使用更高迭代次数的旧版部署过，升级后首次登录会自动检测并重建账号（**密码将重置为 `ADMIN_PASS` 初始值**，旧 token 一并吊销），详见下方"忘记密码"。

## 登录与安全

- 密码：KV 只存 **PBKDF2-SHA256 哈希 + 随机盐**，不存明文
- Token：**HMAC-SHA256 无状态签名**，有效期 24 小时；密钥 `AUTH_SECRET` 通过 `wrangler secret` 注入
- Token 吊销：payload 内含版本号，`POST /api/logout`（退出登录）会递增版本号，**所有端的现有 token 立即失效**
- 登录限流：同一 IP 15 分钟内失败满 **10 次**即锁定到窗口结束（429），计数存 KV；成功登录自动清零
- CORS：默认**仅同源**（前端与 Worker 同域部署，无需跨域头）；确有跨域需求时在 `wrangler.toml` 配 `[vars] ALLOWED_ORIGINS = "https://a.example.com,https://b.example.com"`
- 安全响应头：API 全部返回 `no-store / nosniff / Referrer-Policy`；静态资源由 `public/_headers` 注入 CSP、`X-Frame-Options: DENY` 等
- 静态页面本体公开（浏览器需加载它才能显示登录框），**所有数据 API 均需 `Authorization: Bearer`**，token 无效/过期/已吊销返回 401 并强制回登录页
- 登录失败统一回 `bad credentials`，不泄露账号是否存在
### MQTT 通道安全（重要）

码库 API 有认证，但**设备遥控走的是 MQTT broker**——broker 若匿名开放，任何客户端都能窃取红外码、回放按键操控设备：

- broker **必须启用用户名/密码认证**，并配置 ACL（前端账号只允许读写 `ir-web/#`，设备账号同理收窄）
- HTTPS 页面下前端**强制 `wss://`**（明文 `ws://` 会被拦截并提示）
- 前端连接时若未填 broker 账号密码会拒绝连接并提示
- **MQTT 连接配置云端加密存取**：地址/用户名/主题/密码通过 `GET|PUT /api/mqtt-config` 存入 KV（需 Bearer token），其中**密码用 AES-GCM 加密**（密钥由 `AUTH_SECRET` 经 HKDF 派生，不直接复用 token 密钥材料），KV 中不落明文
- 浏览器发起 MQTT 连接必须持有明文密码，因此 GET 接口会把解密后的密码下发给已登录用户；token 吊销后接口即不可访问。更换 `AUTH_SECRET` 后已存密文无法解密，按无密码处理，重新填写保存即可
- 公网部署建议：broker 仅监听内网/VPN，或经 Cloudflare Tunnel 接入；多用户共用一个页面时注意命令响应按 id 匹配（已带随机前缀防串扰）

### 忘记密码

- **本地 dev**：删除本地 KV 认证记录后重启（`Remove-Item -Recurse .wrangler`），用当前 `ADMIN_PASS` 重新登录
- **生产**：`npx wrangler kv key delete --binding=CODE_LIB "auth:user"` 和 `"auth:pass"`，必要时先 `npx wrangler secret put ADMIN_PASS`；下一次登录请求会用新初始密码重新初始化账号（码库 `code:*` 不受影响）
- **从旧版升级**：若旧版本用了超过 **100000** 次的 PBKDF2 迭代，Worker 无法验证旧哈希，会在首次登录时自动删除旧认证记录并按 `ADMIN_PASS` 重建账号（**密码会被重置为 `ADMIN_PASS` 初始值**，旧 token 全部失效）；请改用 `ADMIN_PASS` 登录。若未配置 `ADMIN_PASS`，登录会返回明确的 500 错误提示，配置后重试即可

## 常见问题

| 现象 | 排查 |
|---|---|
| 登录 500 错误 | 检查 `AUTH_SECRET` 和 `ADMIN_PASS` 是否已设置；从旧版本升级（PBKDF2 迭代次数 >100000）会自动重建账号，确保 `ADMIN_PASS` 已配置 |
| 连不上 broker | 设备是否 STA 模式、MQTT 是否启用、broker WS 端口；地址可只填 host，前端自动补 `wss://` 与 `/mqtt`；未填账号密码会被拒绝 |
| 自动连接没生效 | 云端配置需完整（地址 + 账号密码）；到设置页手动连一次即可保存并触发；HTTPS 页面下 `ws://` 地址会被拦截 |
| 学习模式无帧显示 | 用「拉取历史帧」（`frames` 命令）绕过推送帧/主题错配；或点「推送帧: 开」执行 `fpub` |
| 设备状态"已连接 · 探测中" | 等下一次 12s 轮询；确认 `status` 命令在 cmd 主题有响应 |
| 设备掉线但 badge 仍在线 | 正常断开不触发 LWT，靠 `status` 命令轮询最多 12s 判定离线 |
| 回放暂停接收开关无效 | 设备响应 `result.rx_pause_on_play` 会回写界面；确认命令走的是 MQTT 允许列表 |

## License

本项目为 [esp32c3-ir-web-ESP32-C3](https://github.com/wty2019wty/esp32c3-ir-web-ESP32-C3) 的配套前端，随主项目采用 GPL-3.0。
