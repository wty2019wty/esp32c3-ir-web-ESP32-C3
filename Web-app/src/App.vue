<template>
  <div>
    <Login v-if="!authed" @ok="onLoginOk" />

    <template v-else>
      <header class="app-header">
        <h1>📡 IR 万能遥控器</h1>
        <nav class="tabs">
          <button :class="{ active: tab === 'remote' }" @click="tab = 'remote'">遥控面板</button>
          <button :class="{ active: tab === 'learn' }" @click="tab = 'learn'">学习模式</button>
          <button :class="{ active: tab === 'settings' }" @click="tab = 'settings'">设置</button>
        </nav>
        <span :class="badgeClass">{{ badgeText }}</span>
        <button class="sm ghost" title="退出登录" @click="logout">退出</button>
      </header>

      <div v-show="tab === 'remote'">
        <RemotePad @toast="toast" />
        <CodeLibrary ref="lib" @toast="toast" />
      </div>

      <LearnPanel v-show="tab === 'learn'" @saved="onSaved" @toast="toast" />

      <div v-show="tab === 'settings'">
        <ConnectPanel />
        <DeviceStatus />
      </div>
    </template>

    <div v-if="toasts.length" class="toast" style="right:14px; bottom:14px; top:auto; display:block">
      <div v-for="(t, i) in toasts" :key="i">{{ t.text }}</div>
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
import { onStatus, onFrame, onConn, disconnect, sendCmd } from './mqtt'
import { state } from './store'
import { getAuthToken, clearAuth, onUnauthorized } from './kv'

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
  if (!state.conn.connected) return state.conn.state === 'error' ? `错误: ${state.conn.error}` : '未连接'
  if (state.deviceOnline === false) return '已连接 · 设备离线'
  return state.deviceOnline === true ? '已连接 · 设备在线' : '已连接 · 探测中'
})

function toast(text) {
  toasts.value.push({ text, id: Date.now() })
  if (toasts.value.length > 4) toasts.value.shift()
  if (!toastTimer) {
    toastTimer = setTimeout(() => {
      toasts.value = []
      toastTimer = null
    }, 3000)
  }
}

function onLoginOk() {
  authed.value = true
}

function onAuthRequired() {
  authed.value = false
}

function logout() {
  clearAuth()
  disconnect()
  stopPolling()
  state.deviceOnline = null
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
