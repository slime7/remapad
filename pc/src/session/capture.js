// 主机原始输出采集的落盘器：一行一条记录（相对时间、通道、十六进制字节）。
// 数据与核对状态见 docs/ABSTRACTIONS.md。

import { closeSync, mkdirSync, openSync, writeSync } from 'node:fs';

/** 与 Python 的 `+8.3f` 同格式：带符号、三位小数、总宽 8 右对齐。 */
function formatRelative(seconds) {
  const text = `${seconds < 0 ? '-' : '+'}${Math.abs(seconds).toFixed(3)}`;
  return text.padStart(8, ' ');
}

/** 主机原始输出采集的落盘器。
 *
 * 数据是固件在解析与布局转换之前收到的主机输出（震动参数包、指令帧、
 * 复合输出、固件更新记录流），经桥接帧 0x12 到这里。帧头 slot 是设备侧
 * 记录号：跳号说明设备队列满、丢过包，按次数汇总不逐条打断。
 */
export class HostCaptureSink {
  constructor(path, started) {
    this.path = path;
    this.started = started;
    this.count = 0;
    this.gaps = 0;
    this._fd = null;
    this._lastSeq = null;
  }

  open() {
    const parent = this.path.substring(0, Math.max(this.path.lastIndexOf('\\'), this.path.lastIndexOf('/')));
    if (parent) {
      mkdirSync(parent, { recursive: true });
    }
    this._fd = openSync(this.path, 'w');
    const stamp = new Date();
    const pad = (n) => String(n).padStart(2, '0');
    writeSync(this._fd,
      `# remapad host raw capture ${stamp.getFullYear()}-${pad(stamp.getMonth() + 1)}-${pad(stamp.getDate())} `
            + `${pad(stamp.getHours())}:${pad(stamp.getMinutes())}:${pad(stamp.getSeconds())}\n`
            + '# 列：<相对秒> <通道>[句柄] seq=<记录号> <字节数>B[ trunc] <十六进制字节>\n');
  }

  writeRecord(parsed, seq, now) {
    if (this._fd === null) {
      return;
    }
    if (this._lastSeq !== null && seq !== ((this._lastSeq + 1) & 0xff)) {
      this.gaps += 1;
    }
    this._lastSeq = seq;
    this.count += 1;
    const data = parsed.data;
    const marker = parsed.truncated ? ' trunc' : '';
    const hex = [...data].map((b) => b.toString(16).padStart(2, '0')).join(' ');
    const line = `${formatRelative(now - this.started)}s `
            + `${parsed.name}[0x${parsed.channel.toString(16).toUpperCase().padStart(2, '0')}] `
            + `seq=${String(seq).padStart(3, '0')} ${String(data.length).padStart(3, ' ')}B${marker} ${hex}\n`;
    writeSync(this._fd, line);
  }

  /** 收口落盘器，返回总结一行（本来就没开过返回 null）。 */
  close() {
    if (this._fd === null) {
      return null;
    }
    closeSync(this._fd);
    this._fd = null;
    let summary = `采集已保存：${this.path}（${this.count} 条`;
    if (this.gaps) {
      summary += `，${this.gaps} 处跳号（设备队列满丢包）`;
    }
    return summary + '）';
  }
}
