<template>
  <div class="card">
    <div class="row" style="justify-content: space-between; margin-top: 0">
      <h2>遥控面板</h2>
      <span v-if="state.device" class="badge blue">{{ state.device }}</span>
      <span v-else class="badge gray">未选择遥控器</span>
    </div>

    <!-- 设备状态摘要条 -->
    <div v-if="state.status" class="frame-summary" style="margin: 8px 0">
      <span><b>模式</b>{{ state.status.mode || '-' }}</span>
      <span><b>IP</b>{{ ipText }}</span>
      <span><b>载波</b>{{ state.status.carrier_hz || state.carrier }} Hz</span>
      <span v-if="state.status.playing"><b class="badge yellow">回放中…</b></span>
    </div>

    <!-- 遥控器选择：移动端全宽下拉 -->
    <div class="form-field">
      <label>遥控器</label>
      <select v-model="state.device">
        <option value="" disabled>选择遥控器…</option>
        <option v-for="d in devices" :key="d" :value="d">{{ d }}</option>
      </select>
    </div>

    <!-- 按键网格 -->
    <div v-if="activeCodes.length" class="btn-grid" style="margin-top: 12px">
      <button
        v-for="c in activeCodes"
        :key="c.id"
        :class="{ sending: sendingId === c.id }"
        :disabled="!connected"
        @click="send(c)"
        :title="`${c.note || ''}${c.freq ? ' · ' + c.freq + ' Hz' : ''}`"
      >
        {{ sendingId === c.id ? '发送中…' : c.name }}
      </button>
    </div>
    <div v-else-if="devices.length" class="empty-state">
      当前遥控器没有按键，试试其他遥控器或到「学习」页捕获新按键。
    </div>
    <div v-else class="empty-state">
      <span class="empty-icon">📚</span>
      码库还是空的<br />先到「学习」模式捕获红外信号并保存
    </div>

    <!-- 发送记录（可折叠，减少滚动长度） -->
    <details class="collapse" style="margin-top: 14px" :open="false">
      <summary>发送记录{{ state.playLog.length ? ` (${state.playLog.length})` : '' }}</summary>
      <div v-if="state.playLog.length === 0" class="muted" style="padding: 4px 0">暂无</div>
      <template v-else>
        <div class="row" style="justify-content: flex-end; margin: 0">
          <button class="sm ghost" @click="state.playLog = []">清空记录</button>
        </div>
        <div v-for="(log, i) in state.playLog" :key="i" class="log-line">
          <span class="muted mono">{{ fmtTime(log.ts) }}</span>
          <span :class="log.ok ? 'ok' : 'fail'">{{ log.ok ? '✓' : '✗' }}</span>
          <span>{{ log.text }}</span>
        </div>
      </template>
    </details>
  </div>
</template>

<script setup>
import { ref, computed } from 'vue'
import { state, pushPlayLog, fmtTime } from '../store'
import { playHxd, playRaw } from '../mqtt'

const emit = defineEmits(['toast'])
const sendingId = ref(null)

const connected = computed(() => state.conn.connected)
const devices = computed(() => [...new Set(state.codes.map((c) => c.device))])
const activeCodes = computed(() => state.codes.filter((c) => c.device === state.device))
const ipText = computed(() => {
  const s = state.status
  if (!s) return '-'
  return s.sta_ip || s.ap_ip || '-'
})

async function send(c) {
  if (!connected.value) {
    emit('toast', '请先在「设置」页连接 broker', 'fail')
    return
  }
  sendingId.value = c.id
  const freq = c.freq || state.carrier || 38000
  try {
    if (c.type === 'raw') {
      await playRaw(c.durs, freq)
      pushPlayLog(`回放 ${c.name} (raw ${c.durs.length}段 @${freq}Hz)`, true)
    } else {
      await playHxd(c.value, freq)
      pushPlayLog(`回放 ${c.name} (${c.value} @${freq}Hz)`, true)
    }
  } catch (e) {
    pushPlayLog(`回放 ${c.name} 失败: ${e.message}`, false)
  } finally {
    setTimeout(() => (sendingId.value = null), 400)
  }
}
</script>
