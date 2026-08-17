<template>
  <div class="card">
    <div class="row" style="justify-content: space-between">
      <h2 style="margin:0">码库 (KV)</h2>
      <div class="row" style="margin:0">
        <button class="sm ghost" @click="load" :disabled="state.codesLoading">刷新</button>
        <span class="muted">{{ state.codes.length }} 条</span>
      </div>
    </div>

    <div v-if="state.codesLoading" class="muted">加载中…</div>
    <div v-else-if="state.codes.length === 0" class="muted">码库为空。先用「学习模式」捕获并保存按键码。</div>

    <template v-for="group in groups" :key="group.device">
      <div class="row" style="margin-top:10px; margin-bottom:4px">
        <b style="color:#8ab4ff">{{ group.device }}</b>
        <span class="badge gray">{{ group.items.length }} 键</span>
        <button
          class="sm ghost"
          :class="{ active: state.device === group.device }"
          @click="selectDevice(group.device)"
        >
          {{ state.device === group.device ? '当前遥控器' : '设为当前' }}
        </button>
      </div>
      <table>
        <thead>
          <tr><th>按键</th><th>类型</th><th>频率</th><th>备注</th><th style="width:70px"></th></tr>
        </thead>
        <tbody>
          <tr v-for="c in group.items" :key="c.id">
            <td>
              <span class="mono">{{ c.name }}</span>
              <span v-if="c.type === 'hxd'" class="muted mono" style="display:block;font-size:11px">{{ c.value }}</span>
            </td>
            <td>
              <span :class="c.type === 'hxd' ? 'badge green' : 'badge blue'">{{ c.type === 'hxd' ? 'NEC' : 'RAW' }}</span>
            </td>
            <td class="muted">{{ c.freq ? c.freq + ' Hz' : '-' }}</td>
            <td class="muted">{{ c.note || '-' }}</td>
            <td>
              <button class="sm danger" @click="remove(c)">删除</button>
            </td>
          </tr>
        </tbody>
      </table>
    </template>
  </div>
</template>

<script setup>
import { ref, computed, onMounted } from 'vue'
import { state } from '../store'
import { listCodes, removeCode } from '../kv'

const emit = defineEmits(['toast'])

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
    emit('toast', `加载码库失败: ${e.message}`)
  } finally {
    state.codesLoading = false
  }
}

function selectDevice(d) {
  state.device = d
  emit('toast', `当前遥控器: ${d}`)
}

async function remove(c) {
  if (!confirm(`删除 ${c.device} / ${c.name} ？`)) return
  try {
    await removeCode(c.id)
    state.codes = state.codes.filter((x) => x.id !== c.id)
    emit('toast', '已删除')
  } catch (e) {
    emit('toast', `删除失败: ${e.message}`)
  }
}

onMounted(load)
defineExpose({ load })
</script>
