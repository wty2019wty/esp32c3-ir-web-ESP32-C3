// 红外码库 KV API + 登录认证（Cloudflare Worker）
// 依赖 wrangler.toml 的 kv_namespaces 绑定 CODE_LIB，以及 secret AUTH_SECRET
//
// 路由：
//   POST   /api/login               登录，返回签名 token（唯一公开接口）
//   GET    /api/codes               列出全部码（需 Bearer token）
//   GET    /api/codes/:id           取单个
//   PUT    /api/codes/:id           保存/更新
//   DELETE /api/codes/:id           删除
//
// 安全：
//   - 密码只存 PBKDF2-SHA256 哈希 + 随机盐，绝不存明文
//   - token 为 HMAC-SHA256 无状态签名（payload.signature），有效期 24h
//   - 签名密钥 AUTH_SECRET 走 wrangler secret 注入，不进代码与 KV
//   - 登录失败统一 "bad credentials"，不泄露用户是否存在
//   - 首次使用：无账号记录时用 vars ADMIN_USER/ADMIN_PASS 初始化

const KV_PREFIX = 'code:'
const AUTH_PASS_KEY = 'auth:pass'   // {"salt","iter","hash"}
const AUTH_USER_KEY = 'auth:user'
const TOKEN_TTL_MS = 24 * 3600 * 1000
const PBKDF2_ITER = 150000
const MAX_BODY = 64 * 1024

/* ---------------- 基础工具 ---------------- */

function json(res, body, status = 200) {
  return new Response(JSON.stringify(body), {
    status,
    headers: {
      'Content-Type': 'application/json; charset=utf-8',
      'Access-Control-Allow-Origin': '*',
      'Access-Control-Allow-Methods': 'GET, PUT, DELETE, POST, OPTIONS',
      'Access-Control-Allow-Headers': 'Content-Type, Authorization',
      'Cache-Control': 'no-store',
    },
  })
}

function error(res, message, status = 400) {
  return json(res, { error: message }, status)
}

function cors() {
  return new Response(null, {
    status: 204,
    headers: {
      'Access-Control-Allow-Origin': '*',
      'Access-Control-Allow-Methods': 'GET, PUT, DELETE, POST, OPTIONS',
      'Access-Control-Allow-Headers': 'Content-Type, Authorization',
      'Access-Control-Max-Age': '86400',
    },
  })
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

/* ---------------- token（HMAC-SHA256 无状态签名） ---------------- */

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

/* ---------------- 登录 ---------------- */

async function ensureAuthRecord(env) {
  let passRec = await env.CODE_LIB.get(AUTH_PASS_KEY)
  if (passRec) return { user: await env.CODE_LIB.get(AUTH_USER_KEY) || 'admin', passRec: JSON.parse(passRec) }
  // 首次部署初始化：从 vars 读取初始账号
  const initialUser = env.ADMIN_USER || 'admin'
  const initialPass = env.ADMIN_PASS
  if (!initialPass) {
    return { missing: true }
  }
  const salt = crypto.getRandomValues(new Uint8Array(16))
  const rec = { salt: hex(salt), iter: PBKDF2_ITER, hash: await hashPass(initialPass, hex(salt), PBKDF2_ITER) }
  await env.CODE_LIB.put(AUTH_PASS_KEY, JSON.stringify(rec))
  await env.CODE_LIB.put(AUTH_USER_KEY, initialUser)
  return { user: initialUser, passRec: rec }
}

async function handleLogin(request, env) {
  if (!env.AUTH_SECRET) return error(request, 'AUTH_SECRET not configured', 500)
  let body
  try {
    body = await request.json()
  } catch {
    return error(request, 'invalid json')
  }
  const user = String(body.user || '')
  const pass = String(body.pass || '')
  if (!user || !pass) return error(request, 'need user and pass')

  const auth = await ensureAuthRecord(env)
  if (auth.missing) {
    return error(request, '账号未初始化：请配置 ADMIN_PASS 后重试', 500)
  }
  if (user !== auth.user) return error(request, 'bad credentials', 401)
  const h = await hashPass(pass, auth.passRec.salt, auth.passRec.iter)
  if (h !== auth.passRec.hash) return error(request, 'bad credentials', 401)

  const exp = Date.now() + TOKEN_TTL_MS
  const token = await signToken(env.AUTH_SECRET, { sub: user, iat: Date.now(), exp })
  return json(request, { token, expires_in: TOKEN_TTL_MS / 1000, user })
}

async function checkAuth(request, env) {
  const h = request.headers.get('Authorization') || ''
  const token = h.startsWith('Bearer ') ? h.slice(7).trim() : ''
  if (!token) return null
  return await verifyToken(env.AUTH_SECRET, token)
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

function notFound(res, message = 'not found') {
  return json(res, { error: message }, 404)
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
    return json(request, { codes })
  }

  if (!validId(id)) return error(request, 'invalid id')
  const key = KV_PREFIX + id

  // GET /api/codes/:id
  if (request.method === 'GET') {
    const raw = await kv.get(key)
    if (!raw) return notFound(request)
    try {
      return json(request, JSON.parse(raw))
    } catch {
      return error(request, 'corrupt record', 500)
    }
  }

  // PUT /api/codes/:id
  if (request.method === 'PUT') {
    const cl = request.headers.get('content-length')
    if (cl && Number(cl) > MAX_BODY) return error(request, 'body too large', 413)
    let body
    try {
      body = await request.json()
    } catch {
      return error(request, 'invalid json')
    }
    const rec = normalize(body)
    if (!rec) {
      return error(request, '需要 device/name，且 type 为 hxd（含 value）或 raw（含 durs）')
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
    return json(request, rec, 200)
  }

  // DELETE /api/codes/:id
  if (request.method === 'DELETE') {
    await kv.delete(key)
    return json(request, { ok: true })
  }

  return error(request, 'method not allowed', 405)
}

/* ---------------- 入口 ---------------- */

export default {
  async fetch(request, env) {
    const url = new URL(request.url)

    if (request.method === 'OPTIONS') return cors()

    // 登录是唯一公开接口
    if (url.pathname === '/api/login' && request.method === 'POST') {
      return handleLogin(request, env)
    }

    const m = url.pathname.match(/^\/api\/codes(?:\/([^/]+))?$/)
    if (!m) return error(request, 'not found', 404)

    if (!env.AUTH_SECRET) return error(request, 'AUTH_SECRET not configured', 500)
    const auth = await checkAuth(request, env)
    if (!auth) return json(request, { error: 'unauthorized' }, 401)

    return handleCodes(request, env, url, m[1])
  },
}
