<template>
  <div class="card">
    <div class="row" style="justify-content: space-between">
      <h2 style="margin:0">设备状态</h2>
      <span v-if="offline" class="badge red">离线 (LWT)</span>
      <span v-else class="badge blue">{{ modeBadge }}</span>
    </div>

    <div v-if="!state.status && !offline" class="muted">连接 broker 并订阅状态主题后显示…</div>

    <template v-if="state.status && !offline">
      <div class="frame-summary" style="margin-bottom:6px">
        <span><b>模式</b> {{ state.status.mode }}</span>
        <span><b>IP</b> {{ ipText }}</span>
        <span v-if="state.status.sta_ssid"><b>WiFi</b> {{ state.status.sta_ssid }}</span>
        <span v-else-if="state.status.ap_ssid"><b>热点</b> {{ state.status.ap_ssid }}</span>
        <span><b>载波</b> {{ state.status.carrier_hz }} Hz</span>
        <span v-if="state.status.playing"><b>回放中…</b></span>
      </div>
    </template>

    <div class="row">
      <label class="lbl">载波频率 (Hz)</label>
      <input v-model="carrierInput" type="number" style="width:120px" placeholder="38000" />
      <button class="sm" @click="applyCarrier" :disabled="!connected">设置载波</button>

      <span style="flex:1"></span>
      <label class="lbl">回放暂停接收</label>
      <button
        class="sm ghost"
        :class="{ flash: flashRxPause }"
        @click="toggleRxPause"
        :disabled="!connected"
        :title="'当前: ' + (state.status?.rx_pause_on_play ? '开' : '关')"
      >
        {{ state.status?.rx_pause_on_play ? '开' : '关' }}
      </button>
    </div>
  </div>
</template>

<script setup>
import { ref, computed, watch } from 'vue'
import { sendCmd } from '../mqtt'
import { state, pushPlayLog } from '../store'

const carrierInput = ref('38000')
const flashRxPause = ref(false)

watch(
  () => state.status?.carrier_hz,
  (v) => {
    if (v) carrierInput.value = String(v)
  }
)

const offline = computed(() => !!state.status?.offline)
const connected = computed(() => state.conn.connected)
const ipText = computed(() => {
  const s = state.status
  if (!s) return '-'
  return s.sta_ip || s.ap_ip || '-'
})
const modeBadge = computed(() => {
  const s = state.status
  if (!s) return ''
  return s.mode === 'STA' ? 'STA 模式' : s.mode === 'AP' ? 'AP 模式' : (s.mode || '')
})

async function applyCarrier() {
  const f = Number(carrierInput.value)
  if (!Number.isFinite(f) || f <= 0) return
  try {
    const r = await sendCmd('carrier', { freq: Math.round(f) })
    pushPlayLog(`设置载波 ${r.result?.freq || f} Hz`, true)
    state.carrier = f
  } catch (e) {
    pushPlayLog(`设置载波失败: ${e.message}`, false)
  }
}

async function toggleRxPause() {
  const cur = !!state.status?.rx_pause_on_play
  try {
    // 设备响应携带新的开关状态，必须用它回写本地状态：
    // MQTT 通道在 rxpause 后不会主动推送 status，不回写界面就永远不变。
    const r = await sendCmd('rxpause', { enabled: !cur })
    const next =
      typeof r.result?.rx_pause_on_play === 'boolean'
        ? r.result.rx_pause_on_play
        : !cur
    state.status = { ...(state.status || {}), rx_pause_on_play: next }
    pushPlayLog(`回放暂停接收 -> ${next ? '开' : '关'}`, true)
    flashRxPause.value = true
    setTimeout(() => (flashRxPause.value = false), 600)
  } catch (e) {
    pushPlayLog(`rxpause 失败: ${e.message}`, false)
  }
}
</script>
