<template>
  <div class="card">
    <div class="row" style="justify-content: space-between; margin-top: 0">
      <h2>学习模式</h2>
      <button
        class="sm"
        :class="{ active: state.learning }"
        @click="toggleLearning"
        :disabled="!connected"
      >
        {{ state.learning ? '■ 停止监听' : '● 开始监听' }}
      </button>
    </div>

    <!-- 监听中呼吸提示 -->
    <div v-if="state.learning" class="learning-hint">
      <span class="dot"></span>
      监听中 — 按遥控器对准接收头按键
    </div>

    <details class="collapse" style="margin-bottom: 8px">
      <summary>使用说明</summary>
      <div class="muted" style="padding: 4px 0 8px">
        「开始监听」开启捕获会话：清空当前帧、自动打开设备推送帧（<span class="mono">fpub</span>）并拉取一次历史；
        停止监听时自动关闭推送帧。也可单独用「拉取历史帧」读设备 RAM 历史（<span class="mono">frames</span> 命令）。
        设备须处于 STA 模式且 MQTT 已连接同一 broker。
      </div>
    </details>

    <!-- 工具行 -->
    <div class="row" style="margin-top: 0">
      <button class="sm ghost" @click="pullHistory" :disabled="!connected || pulling">
        {{ pulling ? '拉取中…' : '拉取历史帧' }}
      </button>
      <button
        class="sm ghost"
        :class="{ active: framePushOn }"
        @click="toggleFramePush"
        :disabled="!connected"
      >
        推送帧: {{ framePushOn === null ? '?' : framePushOn ? '开' : '关' }}
      </button>
      <span class="muted" style="margin-left: auto">本次 {{ state.frames.length }} 帧</span>
    </div>

    <!-- 最新捕获帧 -->
    <div v-if="state.lastFrame" class="card" style="padding: 10px 12px; background: var(--bg); margin-top: 10px">
      <div class="row" style="justify-content: space-between; margin: 0">
        <b style="font-size: 13px">最新捕获 #{{ state.lastFrame.seq }}</b>
        <span v-if="state.lastFrame.nec?.ok" class="ok-text mono">{{ state.lastFrame.nec.hxd }}</span>
        <span v-else-if="state.lastFrame.nec?.repeat" class="badge yellow">repeat</span>
        <span v-else class="badge gray">解码失败</span>
      </div>
      <div class="frame-summary" style="margin: 8px 0 4px; font-size: 12.5px">
        <span><b>载波</b>{{ state.lastFrame.freq }} Hz</span>
        <span><b>段数</b>{{ state.lastFrame.feat?.seg_count }}</span>
        <span><b>时长</b>{{ ((state.lastFrame.feat?.total_us || 0) / 1000).toFixed(1) }} ms</span>
      </div>
      <pre style="margin-bottom: 0">{{ frameDetail }}</pre>
    </div>

    <!-- 会话帧列表（可折叠） -->
    <details v-if="state.frames.length > 1" class="collapse" :open="framesOpen" @toggle="framesOpen = $event.target.open">
      <summary>捕获列表 ({{ state.frames.length }})</summary>
      <div>
        <div
          v-for="f in state.frames"
          :key="f.seq"
          class="frame-item"
          :class="{ current: f.seq === state.lastFrame?.seq }"
          @click="selectFrame(f)"
        >
          <span class="frame-seq">#{{ f.seq }}</span>
          <span v-if="f.nec?.ok" class="ok-text mono">{{ f.nec.hxd }}</span>
          <span v-else-if="f.nec?.repeat" class="badge yellow">repeat</span>
          <span v-else class="muted">raw</span>
          <span class="muted" style="margin-left: auto">{{ ((f.feat?.total_us || 0) / 1000).toFixed(1) }} ms · {{ f.feat?.seg_count }}段</span>
        </div>
      </div>
    </details>

    <!-- 未开始时的提示 -->
    <div v-if="!state.lastFrame && !state.learning && !pulling" class="empty-state">
      <span class="empty-icon">🎯</span>
      点「开始监听」进入捕获会话<br />或用「拉取历史帧」读取设备 RAM 历史
    </div>

    <!-- 保存表单 -->
    <template v-if="state.lastFrame">
      <div class="form-field">
        <label>遥控器名称</label>
        <input v-model="saveForm.device" type="text" placeholder="如：电视 / 空调-格力" list="dev-list" />
        <datalist id="dev-list">
          <option v-for="d in devices" :key="d" :value="d" />
        </datalist>
      </div>
      <div class="form-field">
        <label>按键名</label>
        <input v-model="saveForm.name" type="text" placeholder="如：电源 / 音量+" />
      </div>
      <div class="row">
        <label class="lbl">类型</label>
        <select v-model="saveForm.type" style="width: auto; flex: 1">
          <option value="hxd" :disabled="!state.lastFrame.nec?.ok">NEC hxd（推荐）</option>
          <option value="raw">原始波形（raw）</option>
        </select>
        <label class="lbl">备注</label>
        <input v-model="saveForm.note" type="text" placeholder="可选" style="flex: 2" />
      </div>
      <button
        style="width: 100%; margin-top: 10px"
        :disabled="!canSave"
        @click="save"
      >
        💾 保存到码库
      </button>
      <div v-if="!canSave" class="muted" style="margin-top: 6px; text-align: center">
        {{ saveHint }}
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
const framesOpen = ref(true)

// fpub 查询 + 开关（MQTT 专用命令，不写 NVS）
async function toggleFramePush() {
  if (!connected.value) {
    emit('toast', '请先连接 broker', 'fail')
    return
  }
  const next = framePushOn.value !== true
  try {
    const r = await setFramePublish(next)
    framePushOn.value = r.result?.publish_frames === true
    emit('toast', `帧推送 -> ${framePushOn.value ? '开' : '关'}`)
  } catch (e) {
    emit('toast', `fpub 失败: ${e.message}`, 'fail')
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

const saveHint = computed(() => {
  if (!state.lastFrame) return ''
  if (!saveForm.device.trim()) return '请填写遥控器名称'
  if (!saveForm.name.trim()) return '请填写按键名'
  if (saveForm.type === 'hxd' && !state.lastFrame.nec?.ok) return '此帧 NEC 解码失败，请改选「原始波形」类型'
  return ''
})

// 从捕获列表点选一帧作为当前保存对象
function selectFrame(f) {
  state.lastFrame = f
}

function toggleLearning() {
  if (!connected.value) {
    emit('toast', '请先连接 broker', 'fail')
    return
  }
  if (state.learning) {
    stopListening()
  } else {
    startListening()
  }
}

async function startListening() {
  // 清空本次会话的帧，从空白开始捕获
  state.frames = []
  state.lastFrame = null
  state.learning = true
  framesOpen.value = true
  // 打开设备推送帧（失败不阻塞：仍可走拉取历史）
  try {
    const r = await setFramePublish(true)
    framePushOn.value = r.result?.publish_frames === true
  } catch {
    framePushOn.value = true
  }
  await pullHistory()
}

async function stopListening() {
  state.learning = false
  // 停止监听即关闭设备推送帧，并用设备返回的真实状态刷新按钮
  try {
    const r = await setFramePublish(false)
    framePushOn.value = r.result?.publish_frames === true
  } catch (e) {
    emit('toast', `关闭推送帧失败: ${e.message}`, 'fail')
  }
  emit('toast', `已停止监听，本次捕获 ${state.frames.length} 帧`)
}

// 用 frames 命令增量拉取设备 RAM 历史帧（不依赖推送帧 / 不受 topic_suffix 影响）
async function pullHistory() {
  if (!connected.value) {
    emit('toast', '请先连接 broker', 'fail')
    return
  }
  if (pulling.value) return
  pulling.value = true
  let since = state.frames.length ? Math.min(...state.frames.map((f) => f.seq)) - 1 : 0
  let total = 0
  try {
    for (let i = 0; i < 8; i++) {
      const r = await sendCmd('frames', { since })
      const res = r.result || {}
      const list = res.frames || []
      // 按 seq 去重后再插入：since 取的是「比本地最旧帧还小」，设备会回传已有帧，
      // 直接入列会导致 v-for 的 :key="f.seq" 重复、计数虚高
      for (const f of list) {
        if (!state.frames.some((x) => x.seq === f.seq)) {
          pushFrame(f)
          total++
        }
      }
      const lastSeq = res.last_seq
      since = typeof lastSeq === 'number' ? lastSeq : since
      if (!res.truncated || list.length === 0) break
    }
    emit('toast', total ? `拉取到 ${total} 帧` : '设备历史为空（先按遥控器对准接收头按键）')
  } catch (e) {
    emit('toast', `拉取历史失败: ${e.message}`, 'fail')
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
    emit('toast', `已保存：${rec.device} / ${rec.name}`, 'ok')
    state.device = rec.device
    saveForm.name = ''
    saveForm.note = ''
  } catch (e) {
    emit('toast', `保存失败: ${e.message}`, 'fail')
  }
}
</script>

<style scoped>
.learning-hint {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 13px;
  color: var(--green-text);
  background: rgba(31, 92, 46, .25);
  border: 1px solid var(--green);
  border-radius: 10px;
  padding: 9px 12px;
  margin-bottom: 8px;
}
.learning-hint .dot {
  width: 9px; height: 9px; border-radius: 50%;
  background: var(--green-text);
  animation: hint-pulse 1.2s ease-in-out infinite;
}
@keyframes hint-pulse { 50% { opacity: .25; transform: scale(.85); } }
</style>
