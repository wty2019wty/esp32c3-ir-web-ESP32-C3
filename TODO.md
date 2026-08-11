# TODO — 代码审查问题修复清单

基于 commit b8cec05 → 1753d9d 的代码审查，以下问题已逐一对照源码验证：
**8 项确认属实，1 项（原第 8 条）经核实不成立（README 已有说明），已撤销。**

> **第 2 轮审查（2026-08-11，当前 HEAD be0a6c6）**：重新通读全部源码后，
> 确认下方原第 1-7 条全部**仍待修复**（状态未变化），并新增 5 条问题，
> 见文末「第 2 轮审查新增」。

---

## 🔴 必须修复（合入前）

### 1. MQTT 命令通道无认证（严重）
- **文件**: `main/app_mqtt.c:530-567`
- **问题**: `mqtt_handle_command` 解析 JSON 后直接调用 `web_rpc_exec` 执行任意 RPC，没有任何 token 校验。任何能向 MQTT broker 命令主题发布消息的客户端都可以执行 `wificfg`、`authcfg`、`logout` 等命令。
- **对比**: WebSocket 通道在 `app_web_ws.c:540` 有 `ws_client_gen_ok(fd)` 检查。
- **修复方案**: 在 `mqtt_handle_command` 中 `cJSON_Parse` 之后增加 token 校验：
  ```c
  cJSON *jtoken = cJSON_GetObjectItem(root, "token");
  if (!cJSON_IsString(jtoken) || !web_auth_token_ok(jtoken->valuestring)) {
      const char *rid = cJSON_IsString(cJSON_GetObjectItem(root, "id"))
                        ? cJSON_GetObjectItem(root, "id")->valuestring : NULL;
      mqtt_respond(NULL, rid, NULL, "unauthorized");
      cJSON_Delete(root);
      return;
  }
  ```
- **补充（验证）**: README.md:408-409 已写明该通道无认证、安全性依赖 Broker 凭据/TLS，
  文档信任边界已存在。若不想破坏现有简单协议（裸命令名、无需 token），
  可先采用**命令白名单**：MQTT 通道禁用 `wificfg` / `authcfg` / `webcfg` / `mqttcfg`
  等安全敏感命令，改动更小且不破坏现有用法。
- **状态**: [ ] 待修复

### 2. 认证 JSON 注入（高）
- **文件**: `main/app_web_auth.c:256-267`
- **问题**: `web_authcfg_get_json` 用 `snprintf` 将 `cfg.user` 直接拼入 JSON，未做转义。用户名含 `"` 或 `\` 时产生非法 JSON。
- **对比**: `web_wificfg_get_json` 和 `web_mqttcfg_get_json` 均使用 cJSON 构建。
- **修复方案**: 改用 cJSON 构建：
  ```c
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "user", cfg.user);
  cJSON_AddBoolToObject(root, "single_session", web_auth_single_session_get());
  char *s = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return s;
  ```
- **补充（验证）**: set 路径（app_web_auth.c:280-286）对用户名**无任何特殊字符校验**
  （直接 `strlcpy`），含引号用户名可真实写入 NVS；且经无认证的 MQTT 通道（见第 1 条）
  也可触发，实际可永久破坏设置页 JSON。
- **状态**: [ ] 待修复

---

## 🟡 建议修复（后续迭代）

### 3. 密码长度错误提示差一
- **文件**: `main/app_web_auth.c:289-290`
- **问题**: 校验 `strlen >= sizeof(cfg.pass)`（65），实际接受 4-64 字符，但提示写 "pass must be 4-63 chars"。
- **修复**: 改为 `"pass must be 4-64 chars"`
- **状态**: [ ] 待修复

### 4. WS 错误响应字符串拼接
- **文件**: `main/app_web_ws.c:567-573`
- **问题**: `snprintf` 直接拼接错误消息到 JSON，当前安全（全是硬编码字面量），但模式脆弱。
- **修复**: 改用 cJSON 构建响应，与 `mqtt_respond` 保持一致。
- **状态**: [ ] 待修复

### 5. `schedule_restart` 重复定义 ×3
- **文件**: `main/app_web.c:32-45`、`main/app_web_api_wifi.c:69-83`、`main/app_mqtt.c:208-224`
- **问题**: 三个文件各自 `static` 定义了功能相同的重启调度函数。
- **修复**: 提取到公共模块（如 `app_web_util.c`），或在 `app_web_internal.h` 中声明共享版本。
- **状态**: [ ] 待修复

### 6. NVS 单次会话读取未缓存
- **文件**: `main/app_web_auth.c:65-76`
- **问题**: `web_auth_single_session_get` 每次调用都 `nvs_open`/`nvs_close`，涉及 flash 读取。
- **修复**: 添加 `static int s_single_session_cache = -1` 缓存，仅在写入时更新。
- **状态**: [ ] 待修复

### 7. TLS 跳过校验缺 UI 安全警告
- **文件**: `sdkconfig.defaults:46`、`main/app_mqtt.c:767`
- **问题**: `tls_skip` 选项在 Web 界面可配置，但缺少显著的安全警告文案。
- **修复**: 在 Web 设置页对"跳过证书校验"选项增加警告说明。
- **状态**: [ ] 待修复

### 8. ~~NVS 加密 eFuse 烧写缺 README 说明~~（验证后撤销）
- **文件**: `sdkconfig.defaults:12-19`、`README.md:124-132`
- **验证结论**: 原判"README 未说明"不成立——README 已有完整 **NVS 加密** 章节，说明
  HMAC 方案、首次启动自动烧写 32 字节随机密钥到 eFuse KEY4、**不可逆**及升级需先擦 flash；
  `sdkconfig.defaults` 也带 `！！！！` 警示标记。文档覆盖到位，**不再列为待办**。

---

## ✅ 做得好的地方（无需修改）

- Token 认证：常量时间比较、session generation 防旧 token 复用、单会话模式可选、登录失败锁定
- WebSocket 异步发送：队列化避免跨任务 socket 写入竞态
- 帧推送背压：`WS_MAX_PENDING` 防慢客户端拖垮系统
- MQTT 帧发送专用任务：不阻塞 IR 捕获任务
- CSP 策略：前端设置了合理的 Content-Security-Policy
- NVS 原子写入：`web_auth_save_all` 单次 commit 保证一致性
- cJSON 使用：WiFi 和 MQTT 配置 JSON 输出正确转义（仅 auth 配置遗漏）

---

## 🆕 第 2 轮审查新增

> 2026-08-11 全量复审（app_main / IR / WiFi / Web / WS / MQTT / 前端 / CI）。
> 第 1 轮未覆盖的新发现，按同样格式记录。

### 9. WS 会话过期不生效，且可绕过单设备登录（严重，新发现）
- **文件**: `main/app_web_auth.c:164-195`（`web_auth_token_ok` / `web_auth_login`）、
  `main/app_web_ws.c:540`（命令通道只校验会话代数）
- **问题**: WS 命令通道 `ws_client_gen_ok(fd)` 只比对会话代数，从不检查 token
  TTL。token 过期只在 `web_auth_token_ok`（重连的 `auth` 消息路径）被检查，
  且该函数过期时只清空 `s_token`、**不递增会话代数**，因此：
  1. 已登录并一直挂着的 WS 连接在 24 小时后仍可继续执行任意命令
     （与 README「过期后需重新登录」的承诺不符）；
  2. token 过期后重新登录走 `s_token[0]=='\0'` 的「首次登录」分支
     （`app_web_auth.c:232`），只签新 token、不 bump 代数，
     **旧连接不会被踢**，单设备登录在此场景下失效。
- **修复**: 过期时调用 `web_auth_invalidate()`（清 token + bump 代数）；
  登录签发新 token 时（首次登录分支）也 bump 代数，保证任何「重发 token」
  都会作废此前全部会话。
- **状态**: [ ] 待修复

### 10. WiFi 配置静默忽略非法 IP 输入
- **文件**: `main/app_web_api_wifi.c:165-177`
- **问题**: `sta_ip/gw/mask/dns` 四个字段在 `parse_ipv4` 失败时静默保留旧值，
  用户以为保存成功，重启后才发现没生效，无任何错误提示。
- **修复**: `parse_ipv4` 失败时返回 `ESP_ERR_INVALID_ARG` 并给出具体字段错误；
  另外 STA 密码（1-7 位）与 SSID 长度未校验，超长被 `strlcpy` 静默截断，
  建议一并补校验。
- **状态**: [ ] 待修复

### 11. 登录失败锁定为全局，可被局域网用户 DoS
- **文件**: `main/app_web_auth.c:217-220`
- **问题**: 连续 5 次失败锁定整个设备 30 秒（全局状态），任意能访问 WS 的人
  故意输错密码即可把管理员锁在门外（轻量 DoS）。
- **修复**: 按来源 IP 计数锁定（WS 握手阶段可取对端 IP），或至少缩短锁定窗口。
- **状态**: [ ] 待修复

### 12. WebSocket 缺少 Origin 校验
- **文件**: `main/app_web_ws.c:435`（`ws_handler` 升级阶段）
- **问题**: 浏览器允许任意站点发起跨源 WebSocket 连接，配合局域网可达的
  设备可做跨站登录尝试 / 脚本攻击（速率限制仅缓解暴力破解）。
- **修复**: 增加可配置的 Origin 白名单校验。注意「前后端分离」特性
  （GitHub Pages 托管页面连设备）依赖跨源连接，故需做成可配置项而非硬校验。
- **状态**: [ ] 待修复

### 13. MQTT 主题后缀 160 字节上限截断边界
- **文件**: `main/app_mqtt.c:73`（`MQTT_TOPIC_EFF_LEN`）、
  `main/app_mqtt.c:253`（`mqtt_effective_topic`）
- **问题**: 生效主题经 `snprintf` 截断到 159 字节；若基础主题较长 + 长
  Client ID，可能静默截断，极端情况下两个设备后缀截断后撞到同一主题。
- **修复**: 加长缓冲（MQTT 主题协议上限 65535 字节）或保存前校验
  「基础主题 + 后缀」总长度并报错。
- **状态**: [ ] 待修复

### 14. TLS 跳过校验的安全警告不够醒目（第 1 轮 #7 的补充核实）
- **文件**: `main/web/index.html:237-238, 264-267`
- **问题**: 第 1 轮判定「缺 UI 安全警告」部分成立：页面已有文字说明
  （「自签名证书或内网 CA 请选跳过校验」），但无醒目的红色/黄色警示，
  普通用户不易意识到这会关闭证书验证（MITM 风险）。
- **修复**: 在「跳过校验」选项旁加显著警告文案，并在保存前二次确认。
- **状态**: [ ] 待修复
