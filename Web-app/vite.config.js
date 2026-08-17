import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

export default defineConfig({
  plugins: [vue()],
  base: './',
  define: {
    global: 'globalThis',
  },
  build: {
    target: 'es2019',
    chunkSizeWarningLimit: 600,
  },
})
