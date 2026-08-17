import { reactive } from 'vue'

// 全局共享状态
export const state = reactive({
  // broker 连接
  conn: { connected: false, state: 'idle', error: '' },
  // 设备实时状态（status 主题）
  status: null,
  // 设备在线判定：null=未知 true=在线 false=离线
  // 由 status 主题（retained/推送/LWT）+ 主动 status 命令轮询共同维护
  deviceOnline: null,
  // 捕获的红外帧（frame 主题）
  learning: false,
  lastFrame: null,
  frames: [],
  // 码库
  codes: [],
  codesLoading: false,
  // 当前选中的遥控器/载波
  device: '',
  carrier: 38000,
  playLog: [],
})

export function pushPlayLog(text, ok) {
  state.playLog.unshift({ text, ok, ts: Date.now() })
  if (state.playLog.length > 50) state.playLog.length = 50
}

export function pushFrame(frame) {
  state.lastFrame = frame
  state.frames.unshift(frame)
  if (state.frames.length > 30) state.frames.length = 30
}

export const fmtTime = (ts) => {
  if (!ts) return '-'
  const d = new Date(ts)
  const p = (n) => String(n).padStart(2, '0')
  return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`
}
