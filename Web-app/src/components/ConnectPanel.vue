<template>
  <div class="card">
    <h2>MQTT 连接</h2>

    <div class="form-field">
      <label>Broker 地址（ws:// 或 wss://）</label>
      <input v-model="form.url" type="text" inputmode="url"
        placeholder="ws://192.168.1.100:8083/mqtt" :disabled="busy"
        autocapitalize="off" autocorrect="off" spellcheck="false" />
    </div>

    <div class="row">
      <div class="form-field" style="flex: 1; margin-bottom: 0">
        <label>用户名</label>
        <input v-model="form.username" type="text" placeholder="可选" :disabled="busy"
          autocapitalize="off" autocomplete="off" spellcheck="false" />
      </div>
      <div class="form-field" style="flex: 1; margin-bottom: 0">
        <label>密码</label>
        <input v-model="form.password" type="password" placeholder="可选" :disabled="busy" />
      </div>
    </div>

    <details class="collapse">
      <summary>主题配置（默认 ir-web/*，与设备侧保持一致）</summary>
      <div class="form-field">
        <label>命令主题 (cmd)</label>
        <input v-model="form.topicCmd" type="text" :disabled="busy"
          autocapitalize="off" spellcheck="false" />
      </div>
      <div class="form-field">
        <label>响应主题 (rsp)</label>
        <input v-model="form.topicRsp" type="text" :disabled="busy"
          autocapitalize="off" spellcheck="false" />
      </div>
      <div class="form-field">
        <label>状态主题 (status)</label>
        <input v-model="form.topicStatus" type="text" :disabled="busy"
          autocapitalize="off" spellcheck="false" />
      </div>
      <div class="form-field">
        <label>红外帧主题 (frame)</label>
        <input v-model="form.topicFrame" type="text" :disabled="busy"
          autocapitalize="off" spellcheck="false" />
      </div>
    </details>

    <button
      style="width: 100%; margin-top: 10px"
      :class="{ danger: connected }"
      @click="toggle"
      :disabled="busy"
    >
      {{ busy ? '连接中…' : connected ? '断开连接' : '连接' }}
    </button>

    <div class="muted" style="margin-top: 10px">
      设备本身是 MQTT 客户端，前端需连接到<b>同一个 broker</b> 的 WebSocket 端口
      （如 EMQX 8083、Mosquitto 9001）。HTTPS 页面必须用 <span class="mono">wss://</span>。
    </div>
  </div>
</template>

<script setup>
import { reactive, ref, computed, onMounted, onBeforeUnmount } from 'vue'
import { connect, disconnect, onConn } from '../mqtt'
import { state } from '../store'

const emit = defineEmits(['toast'])

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

function onConnState(s) {
  state.conn = s
  busy.value = false
}

onMounted(() => onConn(onConnState))
onBeforeUnmount(() => {}) // onConn 回调由 App.vue 统一管理生命周期

const connected = computed(() => state.conn.connected)

function toggle() {
  if (connected.value) {
    disconnect()
    state.conn = { connected: false, state: 'closed', error: '' }
    return
  }
  let url = form.url.trim()
  if (!url) {
    emit('toast', '请填写 Broker 地址', 'fail')
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
</script>
