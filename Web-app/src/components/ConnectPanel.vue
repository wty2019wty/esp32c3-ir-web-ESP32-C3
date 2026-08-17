<template>
  <div class="card">
    <div class="row" style="justify-content: space-between">
      <h2 style="margin:0">MQTT 连接</h2>
      <span :class="badgeClass">{{ badgeText }}</span>
    </div>

    <div class="row">
      <label class="lbl" style="min-width:110px">Broker (ws/wss)</label>
      <input v-model="form.url" type="text" placeholder="ws://192.168.1.100:8083/mqtt" :disabled="busy" />
      <button @click="toggle" :disabled="busy">{{ connected ? '断开' : '连接' }}</button>
    </div>

    <div class="row">
      <label class="lbl" style="min-width:110px">用户名 / 密码</label>
      <input v-model="form.username" type="text" placeholder="（可选）" style="flex:1" :disabled="busy" />
      <input v-model="form.password" type="password" placeholder="（可选）" style="flex:1" :disabled="busy" />
    </div>

    <details style="margin-top:6px">
      <summary class="muted">主题配置（默认 ir-web/*，与设备侧保持一致）</summary>
      <div class="grid2" style="margin-top:8px">
        <label>命令主题 (cmd)</label>
        <input v-model="form.topicCmd" type="text" :disabled="busy" />
        <label>响应主题 (rsp)</label>
        <input v-model="form.topicRsp" type="text" :disabled="busy" />
        <label>状态主题 (status)</label>
        <input v-model="form.topicStatus" type="text" :disabled="busy" />
        <label>红外帧主题 (frame)</label>
        <input v-model="form.topicFrame" type="text" :disabled="busy" />
      </div>
    </details>

    <div class="muted" style="margin-top:8px">
      提示：设备本身是 MQTT 客户端，前端需连接到<b>同一个 broker</b> 的 WebSocket 端口
      （如 EMQX 8083、Mosquitto 9001）。HTTPS 页面必须用 <span class="mono">wss://</span>。
    </div>
  </div>
</template>

<script setup>
import { reactive, ref, computed, onMounted } from 'vue'
import { connect, disconnect, onConn } from '../mqtt'
import { state } from '../store'

const STORE_KEY = 'ir-web-remote-mqtt'
const form = reactive(
  Object.assign(
    {
      url: '',
      username: '',
      password: '',
      topicCmd: 'ir-web/cmd',
      topicRsp: 'ir-web/rsp',
      topicStatus: 'ir-web/status',
      topicFrame: 'ir-web/frame',
    },
    loadStored()
  )
)
const busy = ref(false)

function loadStored() {
  try {
    return JSON.parse(localStorage.getItem(STORE_KEY)) || {}
  } catch {
    return {}
  }
}
function persist() {
  try {
    localStorage.setItem(STORE_KEY, JSON.stringify(form))
  } catch { /* ignore */ }
}

onConn((s) => {
  state.conn = s
  busy.value = false
})

const connected = computed(() => state.conn.connected)
const badgeClass = computed(() =>
  connected ? 'badge green' : state.conn.state === 'error' ? 'badge red' : 'badge gray'
)
const badgeText = computed(() =>
  connected ? '已连接' : state.conn.state === 'error' ? `错误: ${state.conn.error}` : '未连接'
)

function toggle() {
  if (connected.value) {
    disconnect()
    state.conn = { connected: false, state: 'closed', error: '' }
    return
  }
  let url = form.url.trim()
  if (!url) {
    toast('请填写 Broker 地址')
    return
  }
  if (!/^(ws|wss):\/\//.test(url)) {
    url = (location.protocol === 'https:' ? 'wss://' : 'ws://') + url
  }
  persist()
  busy.value = true
  connect({
    url,
    username: form.username,
    password: form.password,
    topicCmd: form.topicCmd,
    topicRsp: form.topicRsp,
    topicStatus: form.topicStatus,
    topicFrame: form.topicFrame,
  })
}

function toast(text) {
  const el = document.createElement('div')
  el.className = 'toast'
  el.textContent = text
  document.body.appendChild(el)
  setTimeout(() => el.remove(), 2500)
}
defineExpose({ toast })
</script>
