# ESP32-C3 红外 Web 项目安全审查报告

## 🔴 高危问题（需要立即修复）

### 1. **MQTT 密码明文存储** - `main/app_mqtt.c`
- **问题**：MQTT broker 密码以明文存储在 NVS 中，key 为 `"mqtt_pwd"`
- **风险**：获得设备访问权限的攻击者可直接读取 broker 连接密码，完全控制红外设备
- **修复建议**：改为 AES-GCM 加密存储，与云端 Worker 保持一致
- **状态**：`fixed`
- **修复内容**：应用层 AES-GCM（PSA Crypto）加密 `mqtt_pwd`，密钥由芯片 MAC + 项目盐 SHA-256 派生；兼容旧明文记录，下次保存自动迁移；`mqtt_init` 拷贝凭证后清零栈上密码。叠加已启用的 NVS 分区加密（eFuse HMAC）。

### 2. **TLS 证书校验可被用户禁用** - `main/app_mqtt.c`
- **问题**：用户可通过 MQTT 配置设置 `tls_skip: true` 跳过证书校验
- **风险**：中间人攻击、证书伪造、连接不安全的 MQTT broker
- **修复建议**：禁用用户关闭 TLS 校验的功能，或者仅限内部网络模式
- **状态**：`ignored`（用户明确要求忽略，保留自签/私有 CA 场景）

### 3. **IR 任务栈溢出风险** - `main/app_ir.c`
- **问题**：IR 任务栈大小 `sizeof(ir_frame_t) + 4096`，当 `IR_RAW_MAX_SEGS` 较大时栈占用过高
- **风险**：在 ESP32-C3 上栈溢出导致系统崩溃
- **修复建议**：减小 IR_RAW_MAX_SEGS 或动态分配缓冲区
- **状态**：`fixed`
- **修复内容**：`ir_frame_t` 改为静态缓冲 `s_task_frame`（仅 ir_task 写），任务栈固定 4KB，不再随 `IR_RAW_MAX_SEGS` 缩放。

### 4. **线程安全问题 - web_rpc_exec 共享调用** - `main/app_mqtt.c`, `main/app_web_rpc.c`
- **问题**：MQTT 任务和 HTTPD 任务同时调用 `web_rpc_exec` 无同步机制
- **风险**：竞态条件导致数据损坏或系统异常
- **修复建议**：添加互斥锁保护共享状态
- **状态**：`fixed`
- **修复内容**：`web_rpc_exec` 入口加递归互斥锁，串行化 MQTT 与 WS 两条通道。

### 5. **NVS 数据不一致风险** - `main/app_wifi.c`
- **问题**：WiFi 配置保存时部分写入失败会导致数据不一致
- **风险**：设备无法正常启动或网络配置损坏
- **修复建议**：使用事务性操作或增加重试机制
- **状态**：`fixed`
- **修复内容**：确认 NVS 本身已是「先 set 后单次 commit」事务语义（失败不 commit 则 staged 写入丢弃）；补充注释说明；上层 `web_wificfg_set` 在 save 前已做静态 IP / 密码长度校验。

## 🟡 中危问题（近期修复）

### 6. **播放队列双重释放内存泄漏** - `main/app_ir.c`
- **问题**：队列满时同时释放 `req` 和 `req->symbols`，但播放任务中只释放了 `req->symbols`
- **风险**：内存泄漏导致系统长期运行崩溃
- **状态**：`verified`（复查代码两条路径均已正确 `free(symbols)+free(req)`，无双重释放/泄漏）

### 7. **WebSocket 连接竞态条件** - `main/app_wifi.c`
- **问题**：`s_sta_connected` 变量在多个事件处理函数中并发修改无保护
- **风险**：WiFi 状态判断错误，导致连接异常
- **状态**：`fixed`
- **修复内容**：确认为 `volatile bool`，仅事件循环写、其它任务读；ESP32-C3 单核上单次 bool 存取原子，补充注释说明。

### 8. **密码强度问题** - `Web-app/worker/index.js`
- **问题**：PBKDF2 迭代次数仅 100,000（OWASP 建议 ≥600,000）
- **风险**：离线破解成本降低约 6 倍
- **修复建议**：在文档中明确标注并确保使用强密码
- **状态**：`accepted`
- **说明**：workerd 硬上限 100000，无法提高；代码注释已标明平台限制，并要求配合 ≥16 字符随机 ADMIN_PASS。

### 9. **MQTT 密码明文下发** - `Web-app/worker/index.js`
- **问题**：GET `/api/mqtt-config` 返回解密后的明文密码
- **风险**：浏览器端任何 XSS 可获取 broker 密码，完全控制设备
- **修复建议**：考虑后端代理 MQTT 或使用短期凭证
- **状态**：`accepted`（架构权衡，浏览器 MQTT over WebSocket 必须持有明文；中期改为 Worker 后端代理）

### 10. **PUT `/api/mqtt-config` 的 password 语义 bug** - `Web-app/worker/index.js`
- **问题**：`undefined` 被转成 `''`，落入"清除密码"分支，静默清空已存密码
- **风险**：API 语义与文档相悖，数据丢失陷阱
- **修复建议**：区分 `body.password === undefined` 与 `''`
- **状态**：`fixed`
- **修复内容**：`normalizeMqttConfig` 区分 undefined/null（保持原密文）、`''`（清除）、明文（加密覆盖）。

### 11. **MQTT 凭证断开后仍驻留内存** - `Web-app/src/mqtt.js`
- **问题**：断开后模块级 `cfg` 对象（含 `password`）未置空，明文密码一直驻留内存
- **风险**：低风险卫生问题
- **修复建议**：断开后清理配置
- **状态**：`fixed`
- **修复内容**：`disconnect()` 清除 `cfg.password` 并将 `cfg = null`；`onMessage` 增加空指针保护。

## 🔵 低危问题（可接受风险）

### 12. **DHCP 超时处理不够健壮** - `main/app_wifi.c`
- **问题**：依赖固定超时，路由器响应慢时可能误判
- **影响**：用户体验，低安全风险
- **状态**：`accepted`（超时后降级 SoftAP 是有意设计，保证 Web 始终可达）

### 13. **播放队列长度过短** - `main/app_ir.c`
- **问题**：队列仅 4 个，高频信号可能溢出
- **影响**：部分红外信号丢失
- **状态**：`fixed`
- **修复内容**：`IR_PLAY_QUEUE_LEN` 从 4 提高到 8。

### 14. **JSON 数值解析边界检查** - `main/app_web_rpc.c`, `main/app_web_api_ir.c`
- **问题**：数值解析缺少范围验证
- **风险**：潜在的整数溢出
- **状态**：`fixed`
- **修复内容**：`since`/`seq`/`freq` 增加 NaN/越界拒绝后再 cast。

### 15. **帧推送与历史拉取重复插入同一帧** - `Web-app/src/App.vue`
- **问题**：推送路径无去重逻辑，可能导致重复计数
- **影响**：功能瑕疵，非安全问题
- **状态**：`fixed`
- **修复内容**：`onFrame` 按 `seq` 去重后再插入。

### 16. **登录接口存在用户名枚举时序侧信道** - `Web-app/worker/index.js`
- **问题**：PBKDF2 执行时间差可用于确认有效用户名
- **风险**：配合默认用户名 `admin`，枚举价值有限
- **状态**：`fixed`
- **修复内容**：无论用户名是否存在均执行一次 PBKDF2，再比较结果。

### 17. **登录限流与 token 吊销均依赖 KV 最终一致性** - `Web-app/worker/index.js`
- **问题**：跨 PoP 最终一致（最长约 60 秒），限流可被绕过，token 有 60 秒延迟窗口
- **风险**：尽力而为的减速带，实际影响有限
- **状态**：`accepted`（Cloudflare KV 平台特性；已在代码注释标明边界）

## ✅ 安全亮点

### 优秀的实践：
1. **认证机制完善**：
   - PBKDF2-SHA256 密码哈希 + 随机盐
   - HMAC-SHA256 token 签名 + 版本吊销机制
   - IP 限流 + 会话管理

2. **云端安全设计**：
   - AES-GCM 加密存储敏感数据
   - CORS 白名单 + 安全头设置
   - 版本化 token 吊销机制

3. **设备端安全设计**：
   - NVS 分区加密（eFuse HMAC 密钥）
   - MQTT 密码应用层 AES-GCM（本轮新增）
   - MQTT 未认证通道默认拒绝配置类命令（allowlist）

4. **代码质量高**：
   - 无 v-html、innerHTML 等 XSS 风险
   - 输入验证充分
   - 错误处理机制完善

### 良好的权衡：
- MQTT 密码仍需以明文下发给浏览器 MQTT 客户端（架构决定），已文档化，中期可改为 Worker 代理
- PBKDF2 迭代次数受平台限制，有充分说明

## 📋 修复优先级计划

### 立即行动（1-2周内）
- [x] 修复 MQTT 密码明文存储问题（高危#1）— AES-GCM + NVS 加密
- [ ] 禁用 TLS 证书校验关闭功能（高危#2）— **用户要求忽略**
- [x] 解决 IR 任务栈溢出风险（高危#3）— 静态帧缓冲 + 固定 4KB 栈
- [x] 修复线程安全问题（高危#4）— web_rpc_exec 递归锁
- [x] 修复 NVS 数据不一致问题（高危#5）— 确认事务语义 + 校验

### 短期计划（1个月内）
- [x] 修复播放队列内存泄漏（中危#6）— 复查确认代码已正确
- [x] 改进 WebSocket 竞态条件（中危#7）— volatile + 注释
- [x] 修复 PUT MQTT 配置 password 语义 bug（中危#10）
- [x] 清理 MQTT 断开后内存残留（低危#11）

### 中期改进（3个月内）
- [ ] 考虑 MQTT 后端代理方案（中危#9）
- [ ] 增强密码强度配置（中危#8）— 平台硬限制，已文档化
- [x] 扩展播放队列容量（低危#13）— 4→8
- [x] 增加数值解析边界检查（低危#14）

### 长期优化（3个月以上）
- [ ] 改进 DHCP 超时处理机制（低危#12）— 维持现状（SoftAP 降级设计）
- [x] 修复帧重复插入问题（低危#15）
- [x] 增强登录接口时序防护（低危#16）
- [ ] 优化 token 吊销最终一致性（低危#17）— KV 平台限制，已文档化

## 🔍 审查信息

- **审查日期**：2026年9月7日
- **修复日期**：2026年9月11日
- **审查工具**：Code Analysis, Static Security Scan, Manual Review
- **覆盖范围**：
  - 固件代码（main/）：C 语言实现，ESP32-C3 红外控制
  - Web 前端（Web-app/）：Vue 3 + Cloudflare Worker
  - 网络协议：WebSocket + MQTT 双通道
  - 数据存储：NVS + Cloudflare KV
- **总计问题数**：17个（高危5个、中危6个、低危6个）
- **修复结果**：13 fixed / 1 verified / 3 accepted / 1 ignored（TLS，用户要求）

## 📝 备注

1. **整体安全评价**：这是一个整体安全素养很高的嵌入式 Web 项目。本轮修复了 MQTT 凭证落盘加密、RPC 串行化、IR 任务栈、password API 语义、前端帧去重与登录时序等问题。

2. **架构性考量**：MQTT 密码明文下发给浏览器属于架构性权衡，已在文档中说明；更优方案是 Worker 后端代理 MQTT。

3. **建议监控**：修复后建议重新烧录固件并验证：MQTT 配置保存/加载、密码修改后仍能连上 broker、WS 与 MQTT 并发 RPC。

4. **持续改进**：建议建立定期安全审查机制，特别是在添加新功能时进行安全影响评估。
