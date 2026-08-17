<template>
  <div class="card">
    <div class="row" style="justify-content: space-between">
      <h2 style="margin:0">遥控面板</h2>
      <span v-if="state.device" class="badge blue">{{ state.device }}</span>
      <span v-else class="badge gray">未选择遥控器</span>
    </div>

    <div class="row">
      <label class="lbl">遥控器</label>
      <select v-model="state.device" style="flex:1">
        <option value="" disabled>选择遥控器…</option>
        <option v-for="d in devices" :key="d" :value="d">{{ d }}</option>
      </select>
    </div>

    <div v-if="activeCodes.length" class="btn-grid" style="margin-top:10px">
      <button
        v-for="c in activeCodes"
        :key="c.id"
        :class="{ sending: sendingId === c.id }"
        @click="send(c)"
        :title="`${c.note || ''}${c.freq ? ' · ' + c.freq + ' Hz' : ''}`"
      >
        {{ c.name }}
      </button>
    </div>
    <div v-else class="muted" style="margin-top:8px">
      当前遥控器没有按键。用「学习模式」捕获信号并保存，或在上方码库中设置。
    </div>

    <div style="margin-top:12px">
      <div class="row" style="justify-content: space-between">
        <b style="color:var(--muted);font-size:13px">发送记录</b>
        <button class="sm ghost" @click="state.playLog = []">清空</button>
      </div>
      <div v-if="state.playLog.length === 0" class="muted">暂无</div>
      <div v-for="(log, i) in state.playLog" :key="i" class="log-line">
        <span class="muted">{{ fmtTime(log.ts) }}</span>
        <span :class="log.ok ? 'ok' : 'fail'">{{ log.ok ? '✓' : '✗' }}</span>
        <span>{{ log.text }}</span>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, computed } from 'vue'
import { state, pushPlayLog, fmtTime } from '../store'
import { playHxd, playRaw, sendCmd } from '../mqtt'

const emit = defineEmits(['toast'])
const sendingId = ref(null)

const devices = computed(() => [...new Set(state.codes.map((c) => c.device))])
const activeCodes = computed(() => state.codes.filter((c) => c.device === state.device))

async function send(c) {
  if (!state.conn.connected) {
    emit('toast', '请先连接 broker')
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
