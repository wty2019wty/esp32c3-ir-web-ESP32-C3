// 红外码库 KV API（Cloudflare Worker）
// 依赖 wrangler.toml 中的 kv_namespaces 绑定 CODE_LIB
//
// 路由：
//   GET    /api/codes              列出全部码（可 ?device= 过滤）
//   GET    /api/codes/:id          取单个
//   PUT    /api/codes/:id          保存/更新（body 为码库记录 JSON）
//   DELETE /api/codes/:id          删除
// 所有响应均为 JSON；跨域默认放行（前端可部署在任意域名）。

const KV_PREFIX = 'code:'
const MAX_BODY = 64 * 1024

function json(res, body, status = 200) {
  return new Response(JSON.stringify(body), {
    status,
    headers: {
      'Content-Type': 'application/json; charset=utf-8',
      'Access-Control-Allow-Origin': '*',
      'Access-Control-Allow-Methods': 'GET, PUT, DELETE, OPTIONS',
      'Access-Control-Allow-Headers': 'Content-Type',
      'Cache-Control': 'no-store',
    },
  })
}

function error(res, message, status = 400) {
  return json(res, { error: message }, status)
}

function notFound(res, message = 'not found') {
  return json(res, { error: message }, 404)
}

function validId(id) {
  return !!id && id.length >= 3 && id.length <= 64 && /^[A-Za-z0-9_-]+$/.test(id)
}

// 校验并规整一条码库记录
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

export default {
  async fetch(request, env) {
    const url = new URL(request.url)

    if (request.method === 'OPTIONS') {
      return new Response(null, {
        status: 204,
        headers: {
          'Access-Control-Allow-Origin': '*',
          'Access-Control-Allow-Methods': 'GET, PUT, DELETE, OPTIONS',
          'Access-Control-Allow-Headers': 'Content-Type',
          'Access-Control-Max-Age': '86400',
        },
      })
    }

    // 仅处理 /api/codes 路径
    const m = url.pathname.match(/^\/api\/codes(?:\/([^/]+))?$/)
    if (!m) return error(request, 'not found', 404)

    const id = m[1]
    const kv = env.CODE_LIB
    if (!kv) return error(request, 'KV binding CODE_LIB not configured', 500)

    // ---------- GET /api/codes ----------
    if (request.method === 'GET' && !id) {
      const device = (url.searchParams.get('device') || '').trim()
      let list = []
      let cursor = undefined
      do {
        const page = await kv.list({ prefix: KV_PREFIX, cursor })
        for (const key of page.keys) {
          list.push(key)
        }
        cursor = page.cursor
      } while (cursor)
      const codes = []
      for (const key of list) {
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

    // ---------- GET /api/codes/:id ----------
    if (request.method === 'GET') {
      const raw = await kv.get(key)
      if (!raw) return notFound(request)
      try {
        return json(request, JSON.parse(raw))
      } catch {
        return error(request, 'corrupt record', 500)
      }
    }

    // ---------- PUT /api/codes/:id ----------
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

    // ---------- DELETE /api/codes/:id ----------
    if (request.method === 'DELETE') {
      await kv.delete(key)
      return json(request, { ok: true })
    }

    return error(request, 'method not allowed', 405)
  },
}
