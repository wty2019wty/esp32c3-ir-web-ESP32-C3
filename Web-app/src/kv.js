// Cloudflare Worker KV 码库 API 客户端 + 登录认证。
// Worker 暴露的 REST 接口见 worker/index.js，部署后与前端同源（相对路径 /api）。
const BASE = '/api'

// 认证回调：收到 401 时通知前端登出
let onUnauthorizedHandler = null
export function onUnauthorized(fn) { onUnauthorizedHandler = fn }

// 一条码库记录
// {
//   id: string,          // KV key（uuid）
//   device: string,      // 遥控器/设备名，用于分组
//   name: string,        // 按键名，如 "电源"
//   note: string,        // 备注
//   freq: number,        // 载波频率（Hz）
//   type: 'hxd' | 'raw',
//   value: string,       // hxd 值，如 ED127F80
//   durs: number[],      // raw 原始微秒序列（type=raw 时）
//   created_at: number,
//   updated_at: number,
// }

async function request(method, path, body) {
  const headers = {}
  const token = getAuthToken()
  if (token) headers.Authorization = `Bearer ${token}`
  if (body != null) headers['Content-Type'] = 'application/json'
  let res
  try {
    res = await fetch(`${BASE}${path}`, {
      method,
      headers,
      body: body != null ? JSON.stringify(body) : undefined,
    })
  } catch (e) {
    throw new Error('无法访问码库服务')
  }
  if (res.status === 401) {
    clearAuth()
    if (onUnauthorizedHandler) onUnauthorizedHandler()
    throw new Error('未登录或登录已过期')
  }
  if (!res.ok) {
    let msg = `${res.status} ${res.statusText}`
    try {
      const j = await res.json()
      if (j && j.error) msg = j.error
    } catch { /* ignore */ }
    throw new Error(msg)
  }
  return res.json()
}

/* ---------------- 登录态 ---------------- */

const AUTH_KEY = 'ir-web-remote-auth'

export function getAuthToken() {
  try {
    const s = localStorage.getItem(AUTH_KEY) || ''
    if (!s) return ''
    // 存储格式为 JSON（saveAuth 写入 {"token","user"}），取出其中的 token 字段
    try {
      return JSON.parse(s).token || ''
    } catch {
      return s // 兼容历史明文 token
    }
  } catch {
    return ''
  }
}

export function saveAuth({ token, user }) {
  try {
    localStorage.setItem(AUTH_KEY, JSON.stringify({ token, user }))
  } catch { /* ignore */ }
}

export function getAuthUser() {
  try {
    const s = localStorage.getItem(AUTH_KEY)
    return s ? JSON.parse(s).user : ''
  } catch {
    return ''
  }
}

export function clearAuth() {
  try {
    localStorage.removeItem(AUTH_KEY)
  } catch { /* ignore */ }
}

export async function login(user, pass) {
  const res = await fetch(`${BASE}/login`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ user, pass }),
  })
  let data = null
  try {
    data = await res.json()
  } catch { /* ignore */ }
  if (!res.ok) {
    throw new Error((data && data.error) || `登录失败 (${res.status})`)
  }
  saveAuth({ token: data.token, user: data.user })
  return data
}

// 登出：先通知服务端吊销 token（递增版本号，所有端一起失效），再清本地登录态。
// 网络失败也要继续清理本地，避免"看起来退出了、token 其实还有效"。
export async function logout() {
  try {
    await request('POST', '/logout')
  } catch { /* 服务端不可达也照常清本地 */ }
  clearAuth()
}

/* ---------------- 码库 API ---------------- */

export async function listCodes(device) {
  const q = device ? `?device=${encodeURIComponent(device)}` : ''
  return request('GET', `/codes${q}`)
}

export async function getCode(id) {
  return request('GET', `/codes/${encodeURIComponent(id)}`)
}

export async function saveCode(data) {
  const id = data.id
  return request('PUT', `/codes/${encodeURIComponent(id)}`, data)
}

export async function removeCode(id) {
  return request('DELETE', `/codes/${encodeURIComponent(id)}`)
}

// 生成唯一 id（与 worker 端保持一致：时间戳 + 随机）
export function genId() {
  return `c${Date.now().toString(36)}${Math.random().toString(36).slice(2, 8)}`
}
