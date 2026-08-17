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
import { onStatus, onFrame, onConn, disconnect } from './mqtt'
import { state } from './store'

const connPanel = ref(null)
const lib = ref(null)
const toasts = ref([])
let toastTimer = null

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

onMounted(() => {
  onConn((s) => {
    state.conn = s
  })
  onStatus((data) => {
    state.status = data
    if (data && data.carrier_hz) state.carrier = data.carrier_hz
  })
  onFrame((frame) => {
    // 始终记录帧列表；但只有「监听中」才把最新帧显示到学习面板，
    // 让停止监听/开始监听有可感知的差别
    state.frames.unshift(frame)
    if (state.frames.length > 30) state.frames.length = 30
    if (state.learning) state.lastFrame = frame
  })
})

onBeforeUnmount(() => disconnect())
</script>
