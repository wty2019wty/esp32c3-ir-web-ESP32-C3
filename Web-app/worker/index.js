// 红外码库 KV API + 登录认证（Cloudflare Worker）
// 依赖 wrangler.toml 的 kv_namespaces 绑定 CODE_LIB，以及 secret AUTH_SECRET
//
// 路由：
//   POST   /api/login               登录，返回签名 token（唯一公开接口，带 IP 限流）
//   POST   /api/logout              登出，递增 token 版本号吊销全部现有 token（需 Bearer token）
//   GET    /api/mqtt-config         取 MQTT 连接配置（敏感字段 AES-GCM 加密存 KV，需 Bearer token）
//   PUT    /api/mqtt-config         保存 MQTT 连接配置（需 Bearer token）
//   GET    /api/codes               列出全部码（需 Bearer token）
//   GET    /api/codes/:id           取单个
//   PUT    /api/codes/:id           保存/更新
//   DELETE /api/codes/:id           删除
//
// 安全：
//   - 密码只存 PBKDF2-SHA256 哈希 + 随机盐，绝不存明文
//   - token 为 HMAC-SHA256 无状态签名（payload.signature），有效期 24h；
//     payload 内含版本号 ver，与 KV 中 auth:ver 不一致即视为已吊销（登出/账号重建生效）
//   - 签名密钥 AUTH_SECRET 走 wrangler secret 注入，不进代码与 KV
//   - 登录失败统一 "bad credentials"，不泄露用户是否存在
//   - 登录限流：同一 IP 在 15 分钟窗口内失败满 10 次即锁定至窗口结束（KV 计数）
//     注意：KV 跨 PoP 最终一致（最长约 60s），限流/token 吊销是尽力而为的减速带
//   - CORS 默认仅放行同源；确需跨域时在 [vars] 配置 ALLOWED_ORIGINS（逗号分隔白名单）
//   - 所有响应附带 no-store / nosniff / Referrer-Policy 安全头
//   - 首次使用：无账号记录时用 vars ADMIN_USER/ADMIN_PASS 初始化（并吊销历史 token）

const KV_PREFIX = 'code:'
const AUTH_PASS_KEY = 'auth:pass'   // {"salt","iter","hash"}
const AUTH_USER_KEY = 'auth:user'
const AUTH_VER_KEY = 'auth:ver'     // token 版本号，变更即吊销全部旧 token
const MQTT_CFG_KEY = 'mqtt:config'  // MQTT 连接配置（敏感字段 AES-GCM 加密）
const RL_KEY_PREFIX = 'rl:login:'   // 登录限流计数（按 IP）
const TOKEN_TTL_MS = 24 * 3600 * 1000
// workerd 对 PBKDF2 迭代次数的硬上限为 100000（OWASP 建议 ≥600000）。
// 平台限制无法提高；请配合足够长的随机 ADMIN_PASS（≥16 字符）补偿。
const PBKDF2_MAX_ITER = 100000
const PBKDF2_ITER = PBKDF2_MAX_ITER
const MAX_BODY = 64 * 1024
const RL_WINDOW_MS = 15 * 60 * 1000 // 限流窗口：15 分钟
const RL_MAX_FAILS = 10             // 窗口内允许的最大失败次数

/* ---------------- 基础工具 ---------------- */

// 安全响应头 + 按白名单回显 CORS：
// 默认前后端同域部署，不需要跨域头；只有请求 Origin 与站点同源、
// 或命中 env.ALLOWED_ORIGINS 白名单时才回显 Access-Control-Allow-*。
// 非白名单来源不带 CORS 头，浏览器会直接拦截跨域读取。
function securityHeaders(request, env) {
  const headers = new Headers({
    'Cache-Control': 'no-store',
    'X-Content-Type-Options': 'nosniff',
    'Referrer-Policy': 'no-referrer',
  })
  const origin = request.headers.get('Origin') || ''
  if (!origin) return headers
  const selfOrigin = new URL(request.url).origin
  const extra = String(env.ALLOWED_ORIGINS || '')
    .split(',')
    .map((s) => s.trim())
    .filter(Boolean)
  if (origin !== selfOrigin && !extra.includes(origin)) return headers
  headers.set('Access-Control-Allow-Origin', origin)
  headers.set('Vary', 'Origin')
  headers.set('Access-Control-Allow-Methods', 'GET, PUT, DELETE, POST, OPTIONS')
  headers.set('Access-Control-Allow-Headers', 'Content-Type, Authorization')
  headers.set('Access-Control-Max-Age', '86400')
  return headers
}

function json(request, env, body, status = 200) {
  const headers = securityHeaders(request, env)
  headers.set('Content-Type', 'application/json; charset=utf-8')
  return new Response(JSON.stringify(body), { status, headers })
}

function error(request, env, message, status = 400) {
  return json(request, env, { error: message }, status)
}

const enc = new TextEncoder()
const dec = new TextDecoder()

function b64url(buf) {
  return btoa(String.fromCharCode(...new Uint8Array(buf)))
    .replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '')
}
function unb64url(s) {
  s = s.replace(/-/g, '+').replace(/_/g, '/')
  return Uint8Array.from(atob(s), (c) => c.charCodeAt(0))
}
function hex(buf) {
  return [...new Uint8Array(buf)].map((x) => x.toString(16).padStart(2, '0')).join('')
}
function fromHex(s) {
  return Uint8Array.from(s.match(/../g).map((x) => parseInt(x, 16)))
}

/* ---------------- 密码哈希（PBKDF2-SHA256） ---------------- */

async function hashPass(pass, saltHex, iter) {
  const key = await crypto.subtle.importKey('raw', enc.encode(pass), 'PBKDF2', false, ['deriveBits'])
  const bits = await crypto.subtle.deriveBits(
    { name: 'PBKDF2', hash: 'SHA-256', salt: fromHex(saltHex), iterations: iter },
    key,
    256
  )
  return hex(bits)
}

/* ---------------- token（HMAC-SHA256 无状态签名 + 版本吊销） ---------------- */

async function signToken(secret, payloadObj) {
  const payload = b64url(enc.encode(JSON.stringify(payloadObj)))
  const key = await crypto.subtle.importKey('raw', enc.encode(secret), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign'])
  const sig = await crypto.subtle.sign('HMAC', key, enc.encode(payload))
  return `${payload}.${b64url(sig)}`
}

async function verifyToken(secret, token) {
  const parts = token.split('.')
  if (parts.length !== 2) return null
  const [payload, sig] = parts
  const key = await crypto.subtle.importKey('raw', enc.encode(secret), { name: 'HMAC', hash: 'SHA-256' }, false, ['verify'])
  let valid
  try {
    valid = await crypto.subtle.verify('HMAC', key, unb64url(sig), enc.encode(payload))
  } catch {
    return null
  }
  if (!valid) return null
  try {
    const data = JSON.parse(dec.decode(unb64url(payload)))
    if (!data.exp || Date.now() > data.exp) return null
    return data
  } catch {
    return null
  }
}

// 读取（必要时初始化）token 版本号。单用户模型下全局一个版本即可：
// 版本一变，所有携带旧 ver 的 token 全部失效。
async function getTokenVer(env) {
  let ver = await env.CODE_LIB.get(AUTH_VER_KEY)
  if (!ver) {
    ver = hex(crypto.getRandomValues(new Uint8Array(16)))
    try {
      await env.CODE_LIB.put(AUTH_VER_KEY, ver)
    } catch { /* 写冲突时下次再初始化 */ }
  }
  return ver
}

// 吊销全部现有 token：覆盖为新的随机版本号
async function revokeAllTokens(env) {
  const ver = hex(crypto.getRandomValues(new Uint8Array(16)))
  await env.CODE_LIB.put(AUTH_VER_KEY, ver)
  return ver
}

/* ---------------- 登录限流（KV 固定窗口计数，按 IP） ---------------- */

function clientIp(request) {
  return (
    request.headers.get('CF-Connecting-IP') ||
    (request.headers.get('X-Forwarded-For') || '').split(',')[0].trim() ||
    'unknown'
  )
}

// 返回 { blocked, key, rec }：blocked=true 表示当前窗口内失败次数已达上限
async function loginGate(env, ip) {
  const key = RL_KEY_PREFIX + ip
  try {
    const raw = await env.CODE_LIB.get(key)
    if (raw) {
      const rec = JSON.parse(raw)
      if (rec.until > Date.now()) {
        return { blocked: rec.n >= RL_MAX_FAILS, key, rec }
      }
    }
  } catch { /* 记录损坏视为无记录 */ }
  return { blocked: false, key, rec: null }
}

// 记录一次失败：首次失败起算窗口，之后在同一窗口内累加；
// KV 有 1 写/秒/键限制，写失败不阻塞主流程（限流是尽力而为的减速带）
async function loginFail(env, key, rec) {
  const now = Date.now()
  const active = rec && rec.until > now
  const n = active ? rec.n + 1 : 1
  const until = active ? rec.until : now + RL_WINDOW_MS
  try {
    await env.CODE_LIB.put(key, JSON.stringify({ n, until }), {
      expirationTtl: Math.ceil((until - now) / 1000) + 60,
    })
  } catch { /* ignore */ }
}

// 登录成功清空该 IP 的失败计数
async function loginClear(env, key) {
  try {
    await env.CODE_LIB.delete(key)
  } catch { /* ignore */ }
}

/* ---------------- 登录 ---------------- */

async function ensureAuthRecord(env) {
  let migrated = false
  let passRec = await env.CODE_LIB.get(AUTH_PASS_KEY)
  if (passRec) {
    const rec = JSON.parse(passRec)
    if (rec.iter > PBKDF2_MAX_ITER) {
      // 旧版迭代次数超平台上限，Workerd 无法用旧参数验证密码，
      // 只能丢弃旧认证记录并按 ADMIN_PASS 重建账号（密码会被重置为初始值）。
      // 注意：auth:ver 不删除，由下方 revokeAllTokens 覆盖为新版本号即可。
      console.error('[auth] 检测到旧版认证记录（PBKDF2 迭代次数超平台上限），已删除，将按 ADMIN_PASS 重建账号')
      migrated = true
      await env.CODE_LIB.delete(AUTH_PASS_KEY)
      await env.CODE_LIB.delete(AUTH_USER_KEY)
    } else {
      return { user: (await env.CODE_LIB.get(AUTH_USER_KEY)) || 'admin', passRec: rec }
    }
  }
  // 首次部署 / 旧版迁移重建：从 vars 读取初始账号
  const initialUser = env.ADMIN_USER || 'admin'
  const initialPass = env.ADMIN_PASS
  if (!initialPass) {
    return { missing: true, migrated }
  }
  const salt = crypto.getRandomValues(new Uint8Array(16))
  const rec = { salt: hex(salt), iter: PBKDF2_ITER, hash: await hashPass(initialPass, hex(salt), PBKDF2_ITER) }
  await env.CODE_LIB.put(AUTH_PASS_KEY, JSON.stringify(rec))
  await env.CODE_LIB.put(AUTH_USER_KEY, initialUser)
  // 账号（重新）初始化意味着旧凭证作废：同步吊销全部历史 token
  await revokeAllTokens(env)
  return { user: initialUser, passRec: rec, migrated }
}

async function handleLogin(request, env) {
  if (!env.AUTH_SECRET) return error(request, env, 'AUTH_SECRET not configured', 500)
  let body
  try {
    body = await request.json()
  } catch {
    return error(request, env, 'invalid json')
  }
  const user = String(body.user || '')
  const pass = String(body.pass || '')
  if (!user || !pass) return error(request, env, 'need user and pass')

  // 先过限流闸门再执行 PBKDF2，防止攻击者借登录接口烧 CPU 配额
  const gate = await loginGate(env, clientIp(request))
  if (gate.blocked) {
    return json(request, env, { error: '失败次数过多，请 15 分钟后再试' }, 429)
  }

  const auth = await ensureAuthRecord(env)
  if (auth.missing) {
    const msg = auth.migrated
      ? '检测到旧版认证记录（PBKDF2 迭代次数超限）需重建账号，请配置 ADMIN_PASS secret 后重试'
      : '账号未初始化：请配置 ADMIN_PASS 后重试'
    return error(request, env, msg, 500)
  }

  // 始终执行 PBKDF2（即使用户名不存在），避免用耗时差枚举有效账号
  const h = await hashPass(pass, auth.passRec.salt, auth.passRec.iter)
  const ok = (user === auth.user) && (h === auth.passRec.hash)
  if (!ok) {
    await loginFail(env, gate.key, gate.rec)
    return error(request, env, 'bad credentials', 401)
  }
  await loginClear(env, gate.key)

  const exp = Date.now() + TOKEN_TTL_MS
  const ver = await getTokenVer(env)
  const token = await signToken(env.AUTH_SECRET, { sub: user, iat: Date.now(), exp, ver })
  const payload = { token, expires_in: TOKEN_TTL_MS / 1000, user }
  if (auth.migrated) {
    // 本次登录刚完成旧版记录重建，明确告知密码已重置，避免用户误以为凭证异常
    payload.notice = '检测到旧版认证记录，账号已重建，密码已重置为 ADMIN_PASS 初始值'
  }
  return json(request, env, payload)
}

async function checkAuth(request, env) {
  const h = request.headers.get('Authorization') || ''
  const token = h.startsWith('Bearer ') ? h.slice(7).trim() : ''
  if (!token) return null
  const data = await verifyToken(env.AUTH_SECRET, token)
  if (!data) return null
  // 吊销校验：ver 与 KV 当前值不一致（已登出/账号重建）一律拒绝
  if (data.ver !== (await getTokenVer(env))) return null
  return data
}

/* ---------------- 码库 CRUD ---------------- */

function validId(id) {
  return !!id && id.length >= 3 && id.length <= 64 && /^[A-Za-z0-9_-]+$/.test(id)
}

function normalize(rec) {
  const out = {}
  if (!rec || typeof rec !== 'object') return null
  out.device = String(rec.device || '').trim().slice(0, 64)
  out.name = String(rec.name || '').trim().slice(0, 64)
  out.note = String(rec.note || '').trim().slice(0, 256)
  if (!out.device || !out.name) return null

  const freq = Number(rec.freq)
  out.freq = Number.isFinite(freq) && freq > 0 ? Math.round(freq) : null

  const type = rec.type
  if (type === 'hxd') {
    const value = String(rec.value || '').trim()
    if (!/^[0-9A-Fa-f]{1,8}$/.test(value)) return null
    out.type = 'hxd'
    out.value = value.toUpperCase()
  } else if (type === 'raw') {
    if (!Array.isArray(rec.durs) || rec.durs.length === 0 || rec.durs.length > 4096) return null
    const durs = []
    for (const v of rec.durs) {
      const n = Number(v)
      if (!Number.isFinite(n) || n <= 0 || n > 65000) return null
      durs.push(Math.round(n))
    }
    out.type = 'raw'
    out.durs = durs
  } else {
    return null
  }
  return out
}

function notFound(request, env, message = 'not found') {
  return json(request, env, { error: message }, 404)
}

async function handleCodes(request, env, url, id) {
  const kv = env.CODE_LIB

  // GET /api/codes
  if (request.method === 'GET' && !id) {
    const device = (url.searchParams.get('device') || '').trim()
    let keys = []
    let cursor = undefined
    do {
      const page = await kv.list({ prefix: KV_PREFIX, cursor })
      keys = keys.concat(page.keys)
      cursor = page.cursor
    } while (cursor)
    const codes = []
    for (const key of keys) {
      const raw = await kv.get(key.name)
      if (!raw) continue
      try {
        const rec = JSON.parse(raw)
        if (!device || rec.device === device) codes.push(rec)
      } catch { /* skip corrupt */ }
    }
    codes.sort((a, b) => (a.device === b.device ? a.name.localeCompare(b.name) : a.device.localeCompare(b.device)))
    return json(request, env, { codes })
  }

  if (!validId(id)) return error(request, env, 'invalid id')
  const key = KV_PREFIX + id

  // GET /api/codes/:id
  if (request.method === 'GET') {
    const raw = await kv.get(key)
    if (!raw) return notFound(request, env)
    try {
      return json(request, env, JSON.parse(raw))
    } catch {
      return error(request, env, 'corrupt record', 500)
    }
  }

  // PUT /api/codes/:id
  if (request.method === 'PUT') {
    const cl = request.headers.get('content-length')
    if (cl && Number(cl) > MAX_BODY) return error(request, env, 'body too large', 413)
    let body
    try {
      body = await request.json()
    } catch {
      return error(request, env, 'invalid json')
    }
    const rec = normalize(body)
    if (!rec) {
      return error(request, env, '需要 device/name，且 type 为 hxd（含 value）或 raw（含 durs）')
    }
    const now = Date.now()
    const existing = await kv.get(key)
    let created = now
    if (existing) {
      try {
        created = JSON.parse(existing).created_at ?? now
      } catch { /* ignore */ }
    }
    rec.id = id
    rec.created_at = created
    rec.updated_at = now
    await kv.put(key, JSON.stringify(rec))
    return json(request, env, rec, 200)
  }

  // DELETE /api/codes/:id
  if (request.method === 'DELETE') {
    await kv.delete(key)
    return json(request, env, { ok: true })
  }

  return error(request, env, 'method not allowed', 405)
}

/* ---------------- MQTT 连接配置（AES-GCM 加密存储） ---------------- */

// 加密密钥从 AUTH_SECRET 派生（HKDF-SHA256），不直接复用 token 签名密钥材料
async function mqttCfgKey(env) {
  const base = await crypto.subtle.importKey('raw', enc.encode(env.AUTH_SECRET), 'HKDF', false, ['deriveKey'])
  return crypto.subtle.deriveKey(
    { name: 'HKDF', hash: 'SHA-256', salt: enc.encode('mqtt-config-v1'), info: enc.encode('mqtt-cred') },
    base,
    { name: 'AES-GCM', length: 256 },
    false,
    ['encrypt', 'decrypt']
  )
}

// 敏感字段（password）AES-GCM 加密，输出 "enc:v1:<iv-b64url>:<ct-b64url>"
async function encryptField(env, plain) {
  const iv = crypto.getRandomValues(new Uint8Array(12))
  const key = await mqttCfgKey(env)
  const ct = await crypto.subtle.encrypt({ name: 'AES-GCM', iv }, key, enc.encode(plain))
  return `enc:v1:${b64url(iv)}:${b64url(ct)}`
}

async function decryptField(env, value) {
  const m = /^enc:v1:([^:]+):(.+)$/.exec(value || '')
  if (!m) return value || '' // 兼容未加密的历史记录
  try {
    const key = await mqttCfgKey(env)
    const pt = await crypto.subtle.decrypt({ name: 'AES-GCM', iv: unb64url(m[1]) }, key, unb64url(m[2]))
    return dec.decode(pt)
  } catch {
    return '' // 解密失败（如 AUTH_SECRET 已更换）：按无密码处理，让用户重新填写
  }
}

// 校验并规范化 MQTT 配置。url/username 明文存储（非机密），password 加密存储。
// existing: 当前 KV 中的配置（用于 password 省略时保持原值）
async function normalizeMqttConfig(env, body, existing) {
  if (!body || typeof body !== 'object') return null
  const out = {}
  out.url = String(body.url || '').trim().slice(0, 256)
  if (!out.url) return null
  out.username = String(body.username || '').trim().slice(0, 64)
  // 语义：
  //   undefined/null → 保持服务端原密文不动（不能用 String() 转成 ''）
  //   ''             → 显式清除密码
  //   其他字符串     → 新明文，加密后覆盖；已加密的 enc:v1: 原样保留
  if (body.password === undefined || body.password === null) {
    out.password = existing && existing.password != null ? existing.password : ''
  } else if (typeof body.password !== 'string') {
    return null
  } else if (body.password === '') {
    out.password = ''
  } else if (!/^enc:v1:/.test(body.password)) {
    out.password = await encryptField(env, body.password)
  } else {
    out.password = body.password
  }
  const topics = body.topics && typeof body.topics === 'object' ? body.topics : {}
  out.topics = {}
  for (const k of ['cmd', 'rsp', 'status', 'frame']) {
    out.topics[k] = String(topics[k] || '').trim().slice(0, 128)
  }
  return out
}

async function handleMqttConfig(request, env) {
  const kv = env.CODE_LIB

  // GET /api/mqtt-config：返回配置；password 解密后原样下发
  // （浏览器经 MQTT over WebSocket 直连 broker 必须持有明文；架构上更优的是
  //  Worker 后端代理 MQTT，避免明文出网 — 见 TODO 中危#9，属中期改造）
  if (request.method === 'GET') {
    const raw = await kv.get(MQTT_CFG_KEY)
    if (!raw) return json(request, env, { config: null })
    try {
      const cfg = JSON.parse(raw)
      cfg.password = await decryptField(env, cfg.password)
      return json(request, env, { config: cfg })
    } catch {
      return error(request, env, 'corrupt record', 500)
    }
  }

  // PUT /api/mqtt-config
  if (request.method === 'PUT') {
    const cl = request.headers.get('content-length')
    if (cl && Number(cl) > MAX_BODY) return error(request, env, 'body too large', 413)
    let body
    try {
      body = await request.json()
    } catch {
      return error(request, env, 'invalid json')
    }
    let existing = null
    try {
      const raw = await kv.get(MQTT_CFG_KEY)
      if (raw) existing = JSON.parse(raw)
    } catch { existing = null }
    const rec = await normalizeMqttConfig(env, body, existing)
    if (!rec) return error(request, env, '需要 url 字段')
    // 前端传 "enc:v1:..." 表示原样保留服务端密文（避免明文回传往返）；否则是新增密
    rec.updated_at = Date.now()
    await kv.put(MQTT_CFG_KEY, JSON.stringify(rec))
    return json(request, env, { ok: true })
  }

  return error(request, env, 'method not allowed', 405)
}

/* ---------------- 入口 ---------------- */

export default {
  async fetch(request, env) {
    try {
      const url = new URL(request.url)

      if (request.method === 'OPTIONS') {
        return new Response(null, { status: 204, headers: securityHeaders(request, env) })
      }

      if (url.pathname === '/api/login' && request.method === 'POST') {
        return await handleLogin(request, env)
      }

      const mCodes = url.pathname.match(/^\/api\/codes(?:\/([^/]+))?$/)
      const isLogout = url.pathname === '/api/logout' && request.method === 'POST'
      const isMqttCfg = url.pathname === '/api/mqtt-config' && (request.method === 'GET' || request.method === 'PUT')
      if (!mCodes && !isLogout && !isMqttCfg) return error(request, env, 'not found', 404)

      if (!env.AUTH_SECRET) return error(request, env, 'AUTH_SECRET not configured', 500)
      const auth = await checkAuth(request, env)
      if (!auth) return json(request, env, { error: 'unauthorized' }, 401)

      if (isLogout) {
        await revokeAllTokens(env)
        return json(request, env, { ok: true })
      }

      if (isMqttCfg) return handleMqttConfig(request, env)

      return handleCodes(request, env, url, mCodes[1])
    } catch (e) {
      console.error('[api] unhandled error:', e)
      return json(request, env, { error: 'server error' }, 500)
    }
  },
}
