<template>
  <div class="card">
    <div class="row" style="justify-content: space-between; margin-top: 0">
      <h2>码库</h2>
      <div class="row" style="margin: 0">
        <span class="muted">{{ state.codes.length }} 条</span>
        <button class="sm ghost" @click="load" :disabled="state.codesLoading">
          {{ state.codesLoading ? '加载中…' : '刷新' }}
        </button>
      </div>
    </div>

    <div v-if="state.codesLoading && !state.codes.length" class="empty-state">加载中…</div>
    <div v-else-if="state.codes.length === 0" class="empty-state">
      <span class="empty-icon">📚</span>
      码库为空<br />先到「学习」模式捕获并保存按键码
    </div>

    <!-- 按遥控器分组的卡片列表（移动端友好，无横向表格） -->
    <template v-for="group in groups" :key="group.device">
      <div class="code-group-title">
        <span>{{ group.device }}</span>
        <span class="badge gray">{{ group.items.length }} 键</span>
        <button
          class="sm ghost"
          :class="{ active: state.device === group.device }"
          @click="selectDevice(group.device)"
          style="margin-left: auto"
        >
          {{ state.device === group.device ? '✓ 当前遥控器' : '设为当前' }}
        </button>
      </div>

      <div class="code-list">
        <div v-for="c in group.items" :key="c.id" class="code-card">
          <div class="code-main">
            <div class="code-name">{{ c.name }}</div>
            <div class="code-meta">
              <span :class="c.type === 'hxd' ? 'badge green' : 'badge blue'">{{ c.type === 'hxd' ? 'NEC' : 'RAW' }}</span>
              <span v-if="c.freq">{{ c.freq }} Hz</span>
              <span v-if="c.note">{{ c.note }}</span>
            </div>
            <div v-if="c.type === 'hxd'" class="code-value">{{ c.value }}</div>
          </div>
          <button
            class="sm"
            :class="{ sending: sendingId === c.id }"
            :disabled="!connected || sendingId === c.id"
            @click="play(c)"
          >
            {{ sendingId === c.id ? '…' : '发送' }}
          </button>
          <button class="sm ghost" style="color: var(--red-text); border-color: var(--red)" @click="remove(c)">X</button>
        </div>
      </div>
    </template>

    <div v-if="!connected && state.codes.length" class="muted" style="margin-top: 10px; text-align: center">
      连接 broker 后可点「发送」直接回放
    </div>
  </div>
</template>

<script setup>
import { ref, computed, onMounted } from 'vue'
import { state } from '../store'
import { listCodes, removeCode } from '../kv'
import { playHxd, playRaw } from '../mqtt'

const emit = defineEmits(['toast'])
const sendingId = ref(null)

const connected = computed(() => state.conn.connected)

const groups = computed(() => {
  const map = new Map()
  for (const c of state.codes) {
    if (!map.has(c.device)) map.set(c.device, [])
    map.get(c.device).push(c)
  }
  return [...map.entries()].map(([device, items]) => ({ device, items }))
})

async function load() {
  state.codesLoading = true
  try {
    const r = await listCodes()
    state.codes = r.codes || []
  } catch (e) {
    emit('toast', `加载码库失败: ${e.message}`, 'fail')
  } finally {
    state.codesLoading = false
  }
}

function selectDevice(d) {
  state.device = d
  emit('toast', `当前遥控器: ${d}`, 'ok')
}

// 仅「发送」按钮触发回放
async function play(c) {
  if (!connected.value) {
    emit('toast', '请先在「设置」页连接 broker', 'fail')
    return
  }
  if (sendingId.value) return
  sendingId.value = c.id
  const freq = c.freq || state.carrier || 38000
  try {
    if (c.type === 'raw') {
      await playRaw(c.durs, freq)
    } else {
      await playHxd(c.value, freq)
    }
    emit('toast', `已发送 ${c.device} / ${c.name}`, 'ok')
  } catch (e) {
    emit('toast', `发送 ${c.name} 失败: ${e.message}`, 'fail')
  } finally {
    setTimeout(() => (sendingId.value = null), 400)
  }
}

async function remove(c) {
  if (!confirm(`删除 ${c.device} / ${c.name} ？`)) return
  try {
    await removeCode(c.id)
    state.codes = state.codes.filter((x) => x.id !== c.id)
    emit('toast', '已删除', 'ok')
  } catch (e) {
    emit('toast', `删除失败: ${e.message}`, 'fail')
  }
}

// 登录门控切换后（Login -> 主界面）CodeLibrary 才挂载，此时自动加载一次
onMounted(load)
defineExpose({ load })
</script>
