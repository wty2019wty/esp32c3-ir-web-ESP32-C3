<template>
  <div class="login-overlay">
    <div class="login-card">
      <h2>📡 IR 万能遥控器</h2>
      <div class="muted" style="margin-bottom:12px">请登录后使用码库</div>
      <div class="grid2">
        <label>用户名</label>
        <input v-model="user" type="text" autocomplete="username" placeholder="admin" @keyup.enter="doLogin" />
        <label>密码</label>
        <input v-model="pass" type="password" autocomplete="current-password" placeholder="••••••" @keyup.enter="doLogin" />
      </div>
      <div v-if="err" class="err" style="margin-top:8px">{{ err }}</div>
      <div class="row" style="justify-content:flex-end; margin-top:12px">
        <button @click="doLogin" :disabled="busy">{{ busy ? '登录中…' : '登录' }}</button>
      </div>
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
const busy = ref(false)

async function doLogin() {
  if (!user.value.trim() || !pass.value) {
    err.value = '请输入用户名和密码'
    return
  }
  err.value = ''
  busy.value = true
  try {
    await login(user.value.trim(), pass.value)
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

<style scoped>
.login-overlay {
  position: fixed; inset: 0; background: rgba(10, 12, 16, .92);
  display: flex; align-items: center; justify-content: center; z-index: 100; padding: 16px;
}
.login-card {
  width: 380px; max-width: 100%;
  background: var(--card); border: 1px solid var(--border); border-radius: 12px;
  padding: 24px;
}
.login-card h2 { margin: 0 0 4px; color: #fff; }
</style>
