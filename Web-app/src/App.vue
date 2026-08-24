<template>
  <div>
    <Login v-if="!authed" @ok="onLoginOk" />

    <div v-else class="app-shell">
      <header class="app-header">
        <h1>IR 遥控器</h1>

        <!-- 桌面端顶栏导航 -->
        <nav class="desktop-tabs">
          <button :class="{ active: tab === 'remote' }" @click="tab = 'remote'">遥控面板</button>
          <button :class="{ active: tab === 'learn' }" @click="tab = 'learn'">学习模式</button>
          <button :class="{ active: tab === 'settings' }" @click="tab = 'settings'">设置</button>
        </nav>

        <div class="header-right">
          <span :class="badgeClass">{{ badgeText }}</span>
          <button class="sm ghost" @click="logout">退出</button>
        </div>
      </header>

      <main class="app-main">
        <template v-if="tab === 'remote'">
          <RemotePad @toast="toast" />
          <CodeLibrary ref="lib" @toast="toast" />
        </template>

        <LearnPanel v-show="tab === 'learn'" @saved="onSaved" @toast="toast" />

        <template v-if="tab === 'settings'">
          <DeviceStatus />
          <ConnectPanel @toast="toast" />
        </template>
      </main>

      <!-- 移动端底部 Tab 栏 -->
      <nav class="tab-bar">
        <button :class="{ active: tab === 'remote' }" @click="tab = 'remote'">
          <span class="tab-icon">🎮</span>遥控
        </button>
        <button :class="{ active: tab === 'learn' }" @click="tab = 'learn'">
          <span class="tab-icon">📡</span>学习
        </button>
        <button :class="{ active: tab === 'settings' }" @click="tab = 'settings'">
          <span class="tab-icon">⚙️</span>设置
        </button>
      </nav>
    </div>

    <div v-if="toasts.length" class="toast-wrap">
      <div v-for="t in toasts" :key="t.id" class="toast-item" :class="t.kind">{{ t.text }}</div>
    </div>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onBeforeUnmount } from 'vue'
import ConnectPanel from './components/ConnectPanel.vue'
import DeviceStatus from './components/DeviceStatus.vue'
import LearnPanel from './components/LearnPanel.vue'
import CodeLibrary from './components/CodeLibrary.vue'
import RemotePad from './components/RemotePad.vue'
import Login from './components/Login.vue'
import { onStatus, onFrame, onConn, disconnect, sendCmd, connect, normalizeBrokerUrl } from './mqtt'
import { state } from './store'
import { getAuthToken, getMqttConfig, logout as kvLogout, onUnauthorized } from './kv'

const lib = ref(null)
const toasts = ref([])
const tab = ref('remote')
const authed = ref(!!getAuthToken())
let toastTimer = null
let pollTimer = null
let probing = false

const PROBE_INTERVAL_MS = 12000

// 顶部全局状态 badge：broker 连接 + 设备在线
const badgeClass = computed(() => {
  if (!state.conn.connected) return state.conn.state === 'error' ? 'badge red' : 'badge gray'
  if (state.deviceOnline === false) return 'badge yellow'
  return state.deviceOnline === true ? 'badge green' : 'badge blue'
})
const badgeText = computed(() => {
  if (!state.conn.connected) return state.conn.state === 'error' ? `错误` : '未连接'
  if (state.deviceOnline === false) return '设备离线'
  return state.deviceOnline === true ? '设备在线' : '探测中'
})

function toast(text, kind = '') {
  const id = Date.now() + Math.random()
  toasts.value.push({ text, kind, id })
  setTimeout(() => {
    toasts.value = toasts.value.filter((t) => t.id !== id)
  }, 3000)
}

function onLoginOk() {
  authed.value = true
  autoConnect()
}

function onAuthRequired() {
  authed.value = false
}

// 登录成功后自动拉取云端 MQTT 配置并尝试连接：
// 配置完整（url + 账号密码齐全）才连，静默失败不打扰用户（设置页可手动排查）
async function autoConnect() {
  let cfg = null
  try {
    cfg = await getMqttConfig()
  } catch { /* 拉取失败就当没有配置 */ }
  if (!cfg || !cfg.url || !cfg.username || !cfg.password) return
  let url
  try {
    url = normalizeBrokerUrl(cfg.url)
  } catch { return }
  if (!url || state.conn.connected) return
  connect({
    url,
    username: cfg.username,
    password: cfg.password,
    topicCmd: cfg.topics?.cmd,
    topicRsp: cfg.topics?.rsp,
    topicStatus: cfg.topics?.status,
    topicFrame: cfg.topics?.frame,
  })
}

async function logout() {
  stopPolling()
  disconnect()
  state.deviceOnline = null
  // 先请求服务端吊销 token（递增版本号，所有端一起失效），再清本地登录态
  await kvLogout()
  authed.value = false
}

function onSaved() {
  lib.value?.load()
}

// 主动探测设备在线状态：status 命令有响应=在线，超时/失败=离线。
async function probeDevice() {
  if (!state.conn.connected || probing) return
  probing = true
  try {
    const r = await sendCmd('status')
    state.deviceOnline = true
    if (r.result && !r.result.offline) state.status = r.result
    if (r.result?.carrier_hz) state.carrier = r.result.carrier_hz
  } catch {
    state.deviceOnline = false
  } finally {
    probing = false
  }
}

function stopPolling() {
  if (pollTimer) {
    clearInterval(pollTimer)
    pollTimer = null
  }
}

onMounted(() => {
  onUnauthorized(onAuthRequired)
  // 页面刷新时已有有效登录态：直接自动连接（onLoginOk 只覆盖「刚登录」场景）
  if (authed.value) autoConnect()
  onConn((s) => {
    state.conn = s
    if (s.connected) {
      probeDevice()
      if (!pollTimer) pollTimer = setInterval(probeDevice, PROBE_INTERVAL_MS)
    } else {
      stopPolling()
      state.deviceOnline = null
    }
  })
  onStatus((data) => {
    if (data && data.offline) {
      state.deviceOnline = false
    } else {
      state.deviceOnline = true
      state.status = data
      if (data && data.carrier_hz) state.carrier = data.carrier_hz
    }
  })
  onFrame((frame) => {
    state.frames.unshift(frame)
    if (state.frames.length > 30) state.frames.length = 30
    if (state.learning) state.lastFrame = frame
  })
})

onBeforeUnmount(() => {
  stopPolling()
  disconnect()
})
</script>
