// 设备网络链路：netlog UDP 通道（桥接帧 + CLI 文本 + 日志同一端口）。
// 数据与核对状态见 docs/ABSTRACTIONS.md。

import { createSocket } from 'node:dgram';

/** 设备端 UDP 调试通道（netlog）的默认端口。 */
export const NETLOG_PORT_DEFAULT = 9999;

/** 网络连接地址 "ip[:port]" → [ip, port]；端口缺省给 default_port，格式不对抛 Error。 */
export function parseEndpoint(text, defaultPort = NETLOG_PORT_DEFAULT) {
  const trimmed = text.trim();
  const colon = trimmed.indexOf(':');
  const host = (colon < 0 ? trimmed : trimmed.slice(0, colon)).trim();
  if (!host) {
    throw new Error(`地址里没有 IP：${JSON.stringify(text)}`);
  }
  let port = defaultPort;
  if (colon >= 0) {
    const portText = trimmed.slice(colon + 1);
    port = Number(portText);
    if (!Number.isInteger(port) || port < 1 || port > 65535) {
      throw new Error(`端口超出 1-65535：${JSON.stringify(text)}`);
    }
  }
  return [host, port];
}

/**
 * 设备的 UDP 桥接链路：与串口同一模型——桥接帧与 CLI 文本共用一条字节流，靠帧同步字区分。
 * read/write/flush/close 与 SerialLink 同签名，可直接交给 ctrl.Session。
 * UDP 报文不可靠：输入报告与命令丢一拍无感；OTA 的窗口重发能兜住丢包（设备端
 * BEGIN/END 幂等、序号续传），截图这类无重传的大块会话仍走串口。
 */
export class UdpLink {
  /** 单个报文的上限：OTA 一窗 16 帧约 3.6 KB，按 ≤1 KB 切开逐个发送，常见 MTU 下都免分片。 */
  static DATAGRAM_MAX = 1024;

  constructor(host, port) {
    this.address = [host, port];
    this._socket = createSocket('udp4');
    this._inbox = [];
    this._dropped = 0;
    this._error = null;
    this._closed = false;
    this._socket.on('message', (data) => {
      if (this._inbox.length < 256) {
        this._inbox.push(data);
      } else {
        this._dropped += 1;
      }
    });
    this._socket.on('error', (err) => {
      this._error = err;
    });
    this._socket.on('close', () => {
      this._closed = true;
    });
    this._socket.connect(port, host, () => {});
  }

  /** 取出收到的报文；链路层出错时抛出（设备重启后 ICMP 不可达走这里）。 */
  read(_size = 4096) {
    if (this._error) {
      const err = this._error;
      this._error = null;
      throw new Error(`读取 UDP 失败：${err.message}`);
    }
    if (!this._inbox.length) {
      return Buffer.alloc(0);
    }
    return Buffer.concat(this._inbox.splice(0));
  }

  /** 大于一个报文的写入（OTA 的整窗帧）按 DATAGRAM_MAX 切开逐个发送，避免 IP 分片。 */
  write(data) {
    if (this._closed) {
      throw new Error('UDP 链路已关闭');
    }
    for (let offset = 0; offset < data.length; offset += UdpLink.DATAGRAM_MAX) {
      const piece = data.subarray(offset, offset + UdpLink.DATAGRAM_MAX);
      this._socket.send(piece, (err) => {
        if (err) {
          this._error = this._error ?? err;
        }
      });
    }
  }

  /** UDP 没有发送缓冲可等，与串口同签名即可。 */
  flush() {}

  /** UDP 没有驱动接收缓冲可清（旧报文本来就收不到），与串口同签名即可。 */
  purgeInput() {}

  close() {
    if (!this._closed) {
      this._closed = true;
      try {
        this._socket.close();
      } catch {
        // 重复关闭或套接字已失效时静默，与 Python 的收尾路径一致。
      }
    }
  }
}
