// MQTT over WebSocket 封装：负责连接 broker、命令 RPC、状态/帧订阅。
// 设备侧的 esp32c3-ir-web 通过同一个 broker 收发，本模块与设备协议对齐：
//   命令 -> cmd 主题   响应 <- rsp 主题   状态 <- status 主题   红外帧 <- frame 主题
import mqtt from 'mqtt'

export const DEFAULT_TOPICS = {
  cmd: 'ir-web/cmd',
  rsp: 'ir-web/rsp',
  status: 'ir-web/status',
  frame: 'ir-web/frame',
}

let client = null
let seq = 0
let cfg = null
const pending = new Map() // id -> { resolve, reject, timer }
const listeners = {
  status: [],
  frame: [],
  play: [],
  conn: [],
}

function topicFor(role, topic) {
  return topic && topic.trim() ? topic.trim() : DEFAULT_TOPICS[role]
}

function emit(name, payload) {
  for (const fn of listeners[name]) {
    try {
      fn(payload)
    } catch (e) {
      console.error('[mqtt] listener error', e)
    }
  }
}

function onMessage(topic, payloadBuf) {
  const text = payloadBuf.toString()
  if (topic === cfg.topicStatus) {
    // 设备 LWT 遗嘱是裸字符串 "offline"；正常状态是 JSON 对象
    if (text === 'offline') {
      emit('status', { offline: true })
    } else {
      try {
        emit('status', JSON.parse(text))
      } catch { /* ignore */ }
    }
    return
  }
  if (topic === cfg.topicFrame) {
    try {
      emit('frame', JSON.parse(text))
    } catch (e) {
      console.error('[mqtt] bad frame json', e)
    }
    return
  }
  if (topic === cfg.topicRsp) {
    let msg = null
    try {
      msg = JSON.parse(text)
    } catch {
      return
    }
    if (msg && msg.id != null) {
      const p = pending.get(String(msg.id))
      if (p) {
        clearTimeout(p.timer)
        pending.delete(String(msg.id))
        msg.ok ? p.resolve(msg) : p.reject(new Error(msg.error || 'command failed'))
      }
    }
  }
}

// 连接 broker。cfg: { url, username, password, topics, qos }
export function connect(cfgIn) {
  disconnect()
  cfg = {
    url: cfgIn.url,
    username: cfgIn.username || undefined,
    password: cfgIn.password || undefined,
    qos: cfgIn.qos != null ? cfgIn.qos : 1,
    topicCmd: topicFor('cmd', cfgIn.topicCmd),
    topicRsp: topicFor('rsp', cfgIn.topicRsp),
    topicStatus: topicFor('status', cfgIn.topicStatus),
    topicFrame: topicFor('frame', cfgIn.topicFrame),
  }
  client = mqtt.connect(cfg.url, {
    username: cfg.username,
    password: cfg.password,
    clientId: `ir-web-fe-${Math.random().toString(16).slice(2, 10)}`,
    connectTimeout: 10000,
    reconnectPeriod: 5000,
    keepalive: 60,
    clean: true,
  })

  client.on('connect', () => {
    const subs = [
      { topic: cfg.topicCmd },
      { topic: cfg.topicRsp },
      { topic: cfg.topicStatus },
      { topic: cfg.topicFrame },
    ]
    for (const s of subs) {
      client.subscribe(s.topic, { qos: cfg.qos }, (err) => {
        if (err) console.error('[mqtt] subscribe failed', s.topic, err)
      })
    }
    emit('conn', { connected: true })
  })
  client.on('reconnect', () => emit('conn', { connected: false, state: 'reconnecting' }))
  client.on('close', () => emit('conn', { connected: false, state: 'closed' }))
  client.on('error', (e) => emit('conn', { connected: false, state: 'error', error: e.message }))
  client.on('message', onMessage)
}

export function disconnect() {
  if (client) {
    for (const p of pending.values()) {
      clearTimeout(p.timer)
      p.reject(new Error('connection closed'))
    }
    pending.clear()
    try {
      client.end(true)
    } catch { /* ignore */ }
    client = null
  }
}

export function isConnected() {
  return !!client && client.connected
}

// 命令 RPC：发 {"id":"..","cmd":"..","body":{..}} 到 cmd 主题，等待 rsp 主题返回匹配 id。
export function sendCmd(cmd, body, timeout = 8000) {
  return new Promise((resolve, reject) => {
    if (!client || !client.connected) {
      reject(new Error('MQTT 未连接'))
      return
    }
    const id = `c${++seq}`
    const payload = JSON.stringify({ id, cmd, body: body ?? {} })
    const t = setTimeout(() => {
      pending.delete(id)
      reject(new Error(`命令 ${cmd} 超时`))
    }, timeout)
    pending.set(id, { resolve, reject, timer: t })
    client.publish(cfg.topicCmd, payload, { qos: cfg.qos }, (err) => {
      if (err) {
        clearTimeout(t)
        pending.delete(id)
        reject(err)
      }
    })
  })
}

export function playHxd(hxd, freq) {
  return sendCmd('play', { type: 'hxd', value: hxd, ...(freq ? { freq } : {}) })
}

export function playRaw(durs, freq) {
  return sendCmd('play', { type: 'raw', data: durs, ...(freq ? { freq } : {}) })
}

export function playFrame(seqNo, freq) {
  return sendCmd('play', { type: 'frame', seq: seqNo, ...(freq ? { freq } : {}) })
}

export function setCarrier(freq) {
  return sendCmd('carrier', { freq })
}

// 运行时开关 MQTT 帧推送（设备端 fpub 命令，不写 NVS；缺省 enabled 仅查询当前状态）
export function setFramePublish(enabled) {
  return sendCmd('fpub', enabled == null ? {} : { enabled })
}

export function onStatus(fn) { listeners.status.push(fn) }
export function onFrame(fn) { listeners.frame.push(fn) }
export function onPlay(fn) { listeners.play.push(fn) }
export function onConn(fn) { listeners.conn.push(fn) }
