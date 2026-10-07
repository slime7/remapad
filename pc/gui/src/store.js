// 界面共享状态：后端快照轮询进 store，WS 记录进日志环形缓冲。
// 设备与后端是事实源，前端只展示与转发动作。
import { reactive } from 'vue';

import { api, connectRecords } from './api.js';

export const LOG_MAX_LINES = 4000;

export const store = reactive({
  state: null,
  wsOpen: false,
  page: 'session',
  command: '',
  history: [],
  historyIndex: 0,
  log: [],
  logId: 0,
});

// 调试出口：浏览器控制台里可以直接检查界面状态（window.__remapad.store）。
if (typeof window !== 'undefined') {
  window.__remapad = { store };
}

export function applyRecords(items) {
  const stamp = new Date();
  for (const record of items) {
    store.log.push({
      id: store.logId,
      kind: record.kind,
      text: record.text ?? '',
      at: stamp,
    });
    store.logId += 1;
  }
  if (store.log.length > LOG_MAX_LINES) {
    store.log.splice(0, store.log.length - LOG_MAX_LINES);
  }
}

export function logLocal(text) {
  applyRecords([{ kind: 'error', text }]);
}

export async function pollState() {
  try {
    store.state = await api.state();
  } catch {
    // 后端未起或正在重启：保留旧快照，下一拍再试。
  }
}

export function startBackend() {
  connectRecords(applyRecords, (open) => { store.wsOpen = open; });
  pollState();
  setInterval(pollState, 400);
}

// --- 动作封装：历史与命令提交归一处，页面组件直接调 ------------------

export function submitCommand() {
  const text = store.command.trim();
  if (!text) {
    return;
  }
  store.history.push(text);
  store.historyIndex = store.history.length;
  store.command = '';
  api.command(text).catch((exc) => logLocal(`发送失败：${exc.message}`));
}

export function recallHistory(step) {
  if (!store.history.length) {
    return;
  }
  store.historyIndex = Math.max(0, Math.min(store.history.length, store.historyIndex + step));
  store.command = store.historyIndex >= store.history.length ? '' : store.history[store.historyIndex];
}
