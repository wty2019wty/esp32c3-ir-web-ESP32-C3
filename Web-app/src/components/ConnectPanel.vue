<template>
  <div class="card">
    <h2>MQTT 连接</h2>

    <div class="form-field">
      <label>Broker 地址（ws:// 或 wss://）</label>
      <input v-model="form.url" type="text" inputmode="url"
        placeholder="192.168.1.100:8083 或 wss://broker.example.com" :disabled="busy || loading"
        autocapitalize="off" autocorrect="off" spellcheck="false" />
    </div>

    <div class="row">
      <div class="form-field" style="flex: 1; margin-bottom: 0">
        <label>用户名</label>
        <input v-model="form.username" type="text" placeholder="可选" :disabled="busy || loading"
          autocapitalize="off" autocomplete="off" spellcheck="false" />
      </div>
      <div class="form-field" style="flex: 1; margin-bottom: 0">
        <label>密码</label>
        <input v-model="form.password" type="password" placeholder="可选" :disabled="busy || loading" />
      </div>
    </div>

    <details class="collapse">
      <summary>主题配置（默认 ir-web/*，与设备侧保持一致）</summary>
      <div class="form-field">
        <label>命令主题 (cmd)</label>
        <input v-model="form.topicCmd" type="text" :disabled="busy || loading"
          autocapitalize="off" spellcheck="false" />
      </div>
      <div class="form-field">
        <label>响应主题 (rsp)</label>
        <input v-model="form.topicRsp" type="text" :disabled="busy || loading"
          autocapitalize="off" spellcheck="false" />
      </div>
      <div class="form-field">
        <label>状态主题 (status)</label>
        <input v-model="form.topicStatus" type="text" :disabled="busy || loading"
          autocapitalize="off" spellcheck="false" />
      </div>
      <div class="form-field">
        <label>红外帧主题 (frame)</label>
        <input v-model="form.topicFrame" type="text" :disabled="busy || loading"
          autocapitalize="off" spellcheck="false" />
      </div>
    </details>

    <button
      style="width: 100%; margin-top: 10px"
      :class="{ danger: connected }"
      @click="toggle"
      :disabled="busy || loading"
    >
      {{ busy ? '连接中…' : connected ? '断开连接' : '连接' }}
    </button>

    <div class="muted" style="margin-top: 10px">
      设备本身是 MQTT 客户端，前端需连接到<b>同一个 broker</b> 的 WebSocket 端口
      （如 EMQX 8083、Mosquitto 9001）。HTTPS 页面必须用 <span class="mono">wss://</span>。
      <b style="color: var(--red-text)">broker 必须启用账号认证</b>：匿名 broker 上任何客户端都能订阅红外码、回放按键操控设备。
      连接配置（含密码）会<b>AES-GCM 加密</b>存到云端 KV，登录后所有设备自动回填并尝试连接。
    </div>
  </div>
</template>

<script setup>
import { reactive, ref, computed, watch, onMounted } from 'vue'
import { connect, disconnect, normalizeBrokerUrl } from '../mqtt'
import { state } from '../store'
import { getMqttConfig, saveMqttConfig } from '../kv'

const emit = defineEmits(['toast'])

// 云端配置加载中：表单禁用，避免覆盖即将回填的值
const loading = ref(false)

const form = reactive({
  url: '',
  username: '',
  password: '',
  topicCmd: 'ir-web/cmd',
  topicRsp: 'ir-web/rsp',
  topicStatus: 'ir-web/status',
  topicFrame: 'ir-web/frame',
})

async function loadConfig() {
  loading.value = true
  try {
    const cfg = await getMqttConfig()
    if (cfg) {
      form.url = cfg.url || ''
      form.username = cfg.username || ''
      form.password = cfg.password || ''
      form.topicCmd = cfg.topics?.cmd || form.topicCmd
      form.topicRsp = cfg.topics?.rsp || form.topicRsp
      form.topicStatus = cfg.topics?.status || form.topicStatus
      form.topicFrame = cfg.topics?.frame || form.topicFrame
    }
  } catch { /* 拉取失败保持默认空表单，保存时再提示 */ }
  finally {
    loading.value = false
  }
}

onMounted(loadConfig)
defineExpose({ reload: loadConfig })

// state.conn 由 App.vue 统一维护（onConn 只注册一次），这里 watch 引用变化即可：
// 连接成功/失败/断开都会替换 conn 对象，借此复位按钮 busy 态，避免重复注册回调
watch(
  () => state.conn,
  () => {
    busy.value = false
  }
)

const busy = ref(false)
const connected = computed(() => state.conn.connected)

function buildPayload() {
  return {
    url: form.url.trim(),
    username: form.username.trim(),
    // 密码留空字符串表示清除云端已存密码；要保持原密码请勿发空串（API 层省略字段即可）
    password: form.password,
    topics: {
      cmd: form.topicCmd.trim(),
      rsp: form.topicRsp.trim(),
      status: form.topicStatus.trim(),
      frame: form.topicFrame.trim(),
    },
  }
}

async function persist() {
  try {
    await saveMqttConfig(buildPayload())
  } catch (e) {
    emit('toast', `配置保存到云端失败: ${e.message}`, 'fail')
  }
}

async function toggle() {
  if (connected.value) {
    disconnect()
    state.conn = { connected: false, state: 'closed', error: '' }
    return
  }
  let url
  try {
    url = normalizeBrokerUrl(form.url)
  } catch (e) {
    emit('toast', e.message, 'fail')
    return
  }
  if (!url) {
    emit('toast', '请填写 Broker 地址', 'fail')
    return
  }
  if (!form.username || !form.password) {
    emit('toast', 'broker 未配置账号认证，任何能连上 broker 的人都能操控设备', 'fail')
    return
  }
  busy.value = true
  connect({
    url,
    username: form.username,
    password: form.password,
    topicCmd: form.topicCmd,
    topicRsp: form.topicRsp,
    topicStatus: form.topicStatus,
    topicFrame: form.topicFrame,
  })
  // 异步同步到云端 KV；失败不阻断本次连接
  await persist()
}
</script>
