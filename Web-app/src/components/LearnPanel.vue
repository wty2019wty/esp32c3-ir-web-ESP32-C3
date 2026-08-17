<template>
  <div class="card">
    <div class="row" style="justify-content: space-between">
      <h2 style="margin:0">学习模式</h2>
      <div class="row" style="margin:0">
        <button class="sm ghost" @click="toggleFramePush" :disabled="!connected" :class="{ active: framePushOn }">
          推送帧: {{ framePushOn === null ? '未知' : framePushOn ? '开' : '关' }}
        </button>
        <button class="sm ghost" @click="pullHistory" :disabled="!connected || pulling">{{ pulling ? '拉取中…' : '拉取历史帧' }}</button>
        <button class="sm" :class="{ active: state.learning }" @click="toggleLearning" :disabled="!connected">
          {{ state.learning ? '停止监听' : '开始监听' }}
        </button>
      </div>
    </div>
    <div class="muted" style="margin-bottom:8px">
      捕获方式：监听「帧主题」推送（需设备开推送帧，或 <span class="mono">fpub {"enabled":true}</span>）；
      也可用「拉取历史帧」直接读设备 RAM 历史（<span class="mono">frames</span> 命令，无需推送帧）。
      设备须处于 STA 模式且 MQTT 已连接同一 broker。
    </div>

    <div v-if="state.lastFrame" class="card" style="padding:10px 12px; background:var(--bg)">
      <div class="frame-summary">
        <span><b>序号</b> #{{ state.lastFrame.seq }}</span>
        <span><b>NEC</b>
          <span v-if="state.lastFrame.nec?.ok" class="ok mono">{{ state.lastFrame.nec.hxd }}</span>
          <span v-else-if="state.lastFrame.nec?.repeat" class="badge yellow">repeat</span>
          <span v-else class="muted">解码失败</span>
        </span>
        <span><b>载波</b> {{ state.lastFrame.freq }} Hz</span>
        <span><b>段数</b> {{ state.lastFrame.feat?.seg_count }}</span>
        <span><b>时长</b> {{ ((state.lastFrame.feat?.total_us || 0) / 1000).toFixed(1) }} ms</span>
      </div>
      <pre style="margin-top:6px; max-height:120px">{{ frameDetail }}</pre>
    </div>
    <div v-else class="muted">监听中按遥控器按键，捕获的红外帧会显示在这里。</div>

    <template v-if="state.lastFrame">
      <div class="row" style="margin-top:8px">
        <label class="lbl">遥控器</label>
        <input v-model="saveForm.device" type="text" placeholder="如：电视 / 空调-格力" style="flex:1" list="dev-list" />
        <datalist id="dev-list">
          <option v-for="d in devices" :key="d" :value="d" />
        </datalist>
        <label class="lbl">按键名</label>
        <input v-model="saveForm.name" type="text" placeholder="如：电源 / 音量+" style="flex:1" />
      </div>
      <div class="row">
        <label class="lbl">类型</label>
        <select v-model="saveForm.type" style="width:140px">
          <option value="hxd" :disabled="!state.lastFrame.nec?.ok">NEC hxd（推荐）</option>
          <option value="raw">原始波形（raw）</option>
        </select>
        <label class="lbl">备注</label>
        <input v-model="saveForm.note" type="text" placeholder="（可选）" style="flex:1" />
        <button @click="save" :disabled="!canSave" style="margin-left:auto">保存到码库</button>
      </div>
    </template>
  </div>
</template>

<script setup>
import { reactive, ref, computed } from 'vue'
import { state, pushFrame } from '../store'
import { sendCmd, setFramePublish } from '../mqtt'
import { saveCode, genId } from '../kv'

const emit = defineEmits(['saved', 'toast'])

const connected = computed(() => state.conn.connected)
const devices = computed(() => [...new Set(state.codes.map((c) => c.device))])
const pulling = ref(false)
const framePushOn = ref(null)

// fpub 查询 + 开关（MQTT 专用命令，不写 NVS）
async function toggleFramePush() {
  if (!connected.value) {
    emit('toast', '请先连接 broker')
    return
  }
  const next = framePushOn.value !== true
  try {
    const r = await setFramePublish(next)
    framePushOn.value = r.result?.publish_frames === true
    emit('toast', `帧推送 -> ${framePushOn.value ? '开' : '关'}`)
  } catch (e) {
    emit('toast', `fpub 失败: ${e.message}`)
  }
}

const saveForm = reactive({ device: '', name: '', note: '', type: 'hxd' })

const frameDetail = computed(() => {
  const f = state.lastFrame
  if (!f) return ''
  const lines = []
  if (f.nec?.ok) {
    lines.push(`hxd: ${f.nec.hxd}  addr: 0x${f.nec.addr?.toString(16)}  cmd: 0x${f.nec.cmd?.toString(16)}  bits: ${f.nec.bits}`)
  }
  const fe = f.feat
  if (fe) {
    lines.push(`leader: ${fe.leader_pulse}us / ${fe.leader_space}us  pulses: ${fe.pulses}  min/max: ${fe.min_pulse}/${fe.max_pulse}us`)
  }
  lines.push(`durs[${(f.durs || []).length}]: ${(f.durs || []).slice(0, 24).join(',')}${(f.durs || []).length > 24 ? ',…' : ''}`)
  return lines.join('\n')
})

const canSave = computed(() => {
  if (!state.lastFrame) return false
  if (!saveForm.device.trim() || !saveForm.name.trim()) return false
  if (saveForm.type === 'hxd') return !!state.lastFrame.nec?.ok
  return (state.lastFrame.durs || []).length > 0
})

function toggleLearning() {
  state.learning = !state.learning
  if (!state.learning) return
  if (!connected.value) {
    state.learning = false
    emit('toast', '请先连接 broker')
    return
  }
  // 开始监听时先主动拉一次设备历史，避免"等推送没反应"
  pullHistory()
}

// 用 frames 命令增量拉取设备 RAM 历史帧（不依赖推送帧 / 不受 topic_suffix 影响）
async function pullHistory() {
  if (!connected.value) {
    emit('toast', '请先连接 broker')
    return
  }
  if (pulling.value) return
  pulling.value = true
  let since = state.frames.length ? state.frames[0].seq : 0
  let total = 0
  try {
    for (let i = 0; i < 8; i++) {
      const r = await sendCmd('frames', { since })
      const res = r.result || {}
      const list = res.frames || []
      for (const f of list) pushFrame(f)
      total += list.length
      const lastSeq = res.last_seq
      since = typeof lastSeq === 'number' ? lastSeq : since
      if (!res.truncated || list.length === 0) break
    }
    emit('toast', total ? `拉取到 ${total} 帧` : '设备历史为空（先按遥控器对准接收头按键）')
  } catch (e) {
    emit('toast', `拉取历史失败: ${e.message}`)
  } finally {
    pulling.value = false
  }
}

async function save() {
  const f = state.lastFrame
  const rec = {
    id: genId(),
    device: saveForm.device.trim(),
    name: saveForm.name.trim(),
    note: saveForm.note.trim(),
    freq: f.freq || null,
  }
  if (saveForm.type === 'raw') {
    rec.type = 'raw'
    rec.durs = f.durs
  } else {
    rec.type = 'hxd'
    rec.value = f.nec.hxd
  }
  try {
    await saveCode(rec)
    emit('saved', rec)
    emit('toast', `已保存：${rec.device} / ${rec.name}`)
    state.device = rec.device
    saveForm.note = ''
  } catch (e) {
    emit('toast', `保存失败: ${e.message}`)
  }
}
</script>
