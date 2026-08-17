<template>
  <div>
    <h1>📡 IR 万能遥控器 <span class="muted" style="font-size:13px">MQTT over WebSocket · Cloudflare KV 码库</span></h1>

    <ConnectPanel ref="connPanel" />

    <DeviceStatus />

    <div class="grid">
      <div>
        <LearnPanel @saved="onSaved" @toast="toast" />
        <CodeLibrary ref="lib" @toast="toast" />
      </div>
      <div>
        <RemotePad @toast="toast" />
      </div>
    </div>

    <div v-if="toasts.length" class="toast" style="right:14px; bottom:14px; top:auto; display:block">
      <div v-for="(t, i) in toasts" :key="i">{{ t.text }}</div>
    </div>
  </div>
</template>

<script setup>
import { ref, onMounted, onBeforeUnmount } from 'vue'
import ConnectPanel from './components/ConnectPanel.vue'
import DeviceStatus from './components/DeviceStatus.vue'
import LearnPanel from './components/LearnPanel.vue'
import CodeLibrary from './components/CodeLibrary.vue'
import RemotePad from './components/RemotePad.vue'
import { onStatus, onFrame, onConn, disconnect, sendCmd } from './mqtt'
import { state } from './store'

const connPanel = ref(null)
const lib = ref(null)
const toasts = ref([])
let toastTimer = null
let pollTimer = null
let probing = false

const PROBE_INTERVAL_MS = 12000

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

function onSaved() {
  lib.value?.load()
}

// 主动探测设备在线状态：status 命令有响应=在线，超时/失败=离线。
// 这是必要的——设备正常断开（重启）不触发 LWT，仅靠 status 主题会把
// 残留的 retained 在线状态误判为"设备在线"。
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
  onConn((s) => {
    state.conn = s
    if (s.connected) {
      // 连接建立后立刻探测一次，并定时轮询保活判定
      probeDevice()
      if (!pollTimer) pollTimer = setInterval(probeDevice, PROBE_INTERVAL_MS)
    } else {
      stopPolling()
      state.deviceOnline = null
    }
  })
  onStatus((data) => {
    if (data && data.offline) {
      // LWT：设备异常掉线
      state.deviceOnline = false
    } else {
      // retained 在线状态 / 播放变化推送
      state.deviceOnline = true
      state.status = data
      if (data && data.carrier_hz) state.carrier = data.carrier_hz
    }
  })
  onFrame((frame) => {
    // 始终记录帧列表；但只有「监听中」才把最新帧显示到学习面板，
    // 让停止监听/开始监听有可感知的差别
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
