<template>
  <div class="login-overlay">
    <div class="login-card">
      <div class="login-logo">📡</div>
      <h2>IR 万能遥控器</h2>
      <div class="muted" style="margin-bottom: 16px">登录后使用云端红外码库</div>

      <div class="form-field">
        <label for="login-user">用户名</label>
        <input id="login-user" v-model="user" type="text" autocomplete="username"
          placeholder="admin" autocapitalize="off" autocorrect="off"
          spellcheck="false" @keyup.enter="doLogin" />
      </div>
      <div class="form-field">
        <label for="login-pass">密码</label>
        <input id="login-pass" v-model="pass" type="password" autocomplete="current-password"
          placeholder="••••••" @keyup.enter="doLogin" />
      </div>

      <div v-if="err" class="err" style="margin-top: 8px">{{ err }}</div>
      <div v-if="notice" class="muted" style="margin-top: 8px">{{ notice }}</div>

      <button
        style="width: 100%; margin-top: 14px"
        :disabled="busy"
        @click="doLogin"
      >
        {{ busy ? '登录中…' : '登 录' }}
      </button>
    </div>
  </div>
</template>

<script setup>
import { ref } from 'vue'
import { login } from '../kv'

const emit = defineEmits(['ok'])

const user = ref('')
const pass = ref('')
const err = ref('')
const notice = ref('')
const busy = ref(false)

async function doLogin() {
  if (!user.value.trim() || !pass.value) {
    err.value = '请输入用户名和密码'
    return
  }
  err.value = ''
  busy.value = true
  try {
    const res = await login(user.value.trim(), pass.value)
    if (res && res.notice) notice.value = res.notice
    user.value = ''
    pass.value = ''
    emit('ok')
  } catch (e) {
    err.value = e.message
  } finally {
    busy.value = false
  }
}
</script>
