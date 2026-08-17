// Cloudflare Worker KV 码库 API 客户端。
// Worker 暴露的 REST 接口见 worker/index.js，部署后与前端同源（相对路径 /api）。
const BASE = '/api'

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
  const res = await fetch(`${BASE}${path}`, {
    method,
    headers: body != null ? { 'Content-Type': 'application/json' } : undefined,
    body: body != null ? JSON.stringify(body) : undefined,
  })
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
