// 后端 REST 与 WS 客户端：状态走轮询，会话记录走 WS 推流（断线自动重连）。

async function post(path, body = {}) {
  const response = await fetch(`/api/${path}`, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify(body),
  });
  const payload = await response.json().catch(() => ({}));
  if (!response.ok) {
    throw new Error(payload.error ?? response.statusText);
  }
  return payload;
}

export const api = {
  state: () => fetch('/api/state').then((response) => {
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }
    return response.json();
  }),
  connectSerial: (port) => post('connect-serial', { port }),
  connectNet: (net) => post('connect-net', { net }),
  disconnect: () => post('disconnect'),
  refreshPorts: () => post('ports-refresh'),
  refreshPads: () => post('pads-refresh'),
  padCandidates: () => post('pad-candidates'),
  padSelect: (index) => post('pad-select', { index }),
  command: (text) => post('command', { text }),
  forward: (enabled) => post('forward', { enabled }),
  wifiSave: (ssid, pass) => post('wifi-save', { ssid, pass }),
  otaValidate: (path) => post('ota-validate', { path }),
  otaStart: (path, wait) => post('ota-start', { path, wait }),
};

/** 连上 WS 记录通道；掉线后每 2 秒重试，重连成功先回放后端的最近记录。 */
export function connectRecords(onRecords, onStatus = () => {}) {
  let closed = false;
  let socket = null;
  const open = () => {
    if (closed) {
      return;
    }
    const protocol = location.protocol === 'https:' ? 'wss' : 'ws';
    socket = new WebSocket(`${protocol}://${location.host}/ws`);
    socket.onopen = () => onStatus(true);
    socket.onmessage = (event) => {
      const message = JSON.parse(event.data);
      if (message.t === 'records') {
        onRecords(message.items);
      }
    };
    socket.onclose = () => {
      onStatus(false);
      setTimeout(open, 2000);
    };
  };
  open();
  return () => {
    closed = true;
    socket?.close();
  };
}
