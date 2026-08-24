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
      <b style="color: var(--red-text)">broker 必须启用账号认证</b>：匿名 broker 上任何客户端都能订阅红外码、回放按键操控设备。
      密码仅保存在本标签页会话（sessionStorage），刷新后需重填。
    </div>
  </div>
</template>

<script setup>
import { reactive, ref, computed, watch } from 'vue'
import { connect, disconnect } from '../mqtt'
import { state } from '../store'

const emit = defineEmits(['toast'])

const STORE_KEY = 'ir-web-remote-mqtt'
const PASS_KEY = 'ir-web-remote-mqtt-pass'

// broker 密码只存 sessionStorage（标签页关闭即清除），不落 localStorage：
// localStorage 永久驻留且被同源任意脚本可读，明文密码长期暴露面过大
function loadStoredPass() {
  try {
    return sessionStorage.getItem(PASS_KEY) || ''
  } catch {
    return ''
  }
}

const form = reactive(
  Object.assign(
    {
      url: '',
      username: '',
      password: loadStoredPass(),
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
    const stored = JSON.parse(localStorage.getItem(STORE_KEY)) || {}
    delete stored.password // 历史版本曾把密码写进 localStorage，读取时剔除并顺手清除
    return stored
  } catch {
    return {}
  }
}
function persist() {
  try {
    const { password, ...rest } = form
    localStorage.setItem(STORE_KEY, JSON.stringify(rest))
    if (password) sessionStorage.setItem(PASS_KEY, password)
    else sessionStorage.removeItem(PASS_KEY)
  } catch { /* ignore */ }
}

// state.conn 由 App.vue 统一维护（onConn 只注册一次），这里 watch 引用变化即可：
// 连接成功/失败/断开都会替换 conn 对象，借此复位按钮 busy 态，避免重复注册回调
watch(
  () => state.conn,
  () => {
    busy.value = false
  }
)

const connected = computed(() => state.conn.connected)

function normalizeBrokerUrl(raw) {
  let url = raw.trim()
  if (!url) return ''
  if (!/^(ws|wss|mqtt|mqtts):\/\//.test(url)) {
    url = (location.protocol === 'https:' ? 'wss://' : 'ws://') + url
  }
  // mqtt/mqtts scheme 转 ws/wss（浏览器只能走 WebSocket）
  url = url.replace(/^mqtt:\/\//, 'wss://').replace(/^mqtts:\/\//, 'wss://')
  // HTTPS 页面必须 wss：浏览器本就拦截混合内容，这里提前拦截并给出明确提示
  if (location.protocol === 'https:' && url.startsWith('ws://')) {
    throw new Error('HTTPS 页面必须使用 wss:// 连接 broker（明文 ws:// 会被浏览器拦截）')
  }
  // 无路径时自动补默认 WS 端点 /mqtt（EMQX/Mosquitto 惯例）
  try {
    const u = new URL(url)
    if (!u.pathname || u.pathname === '/') {
      u.pathname = '/mqtt'
      url = u.toString()
    }
  } catch { /* 保持原样，让 mqtt.js 报错 */ }
  return url.replace(/\/$/, (m, off) => (off > url.indexOf('/mqtt') ? '' : m))
}

function toggle() {
  if (connected.value) {
    disconnect()
    state.conn = { connected: false, state: 'closed', error: '' }
    return
  }
  let url
  try {
    url = normalizeBrokerUrl(form.url)
  } catch (e) {
    emit('toast', e.message, 'fail')
    return
  }
  if (!url) {
    emit('toast', '请填写 Broker 地址', 'fail')
    return
  }
  if (!form.username || !form.password) {
    emit('toast', 'broker 未配置账号认证，任何能连上 broker 的人都能操控设备', 'fail')
    return
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
