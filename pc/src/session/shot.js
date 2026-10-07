// 实机截图：按偏移拼一块 RGB565 缓冲，收尾后写成 PNG（标准库 zlib，不引新依赖）。
// 数据与核对状态见 docs/ABSTRACTIONS.md。

import { mkdirSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { deflateSync } from 'node:zlib';

import { IMAGE_FORMAT_RGB565_LE, parseImageChunk, parseImageEnd, parseImageInfo } from '../link/frame.js';

/** 默认截图目录（pc/shots）与文件名前缀。 */
export const SHOT_DIR = fileURLToPath(new URL('../../shots/', import.meta.url));
export const SHOT_PREFIX = 'remapad-';

export function defaultShotPath() {
  const stamp = new Date();
  const pad = (n) => String(n).padStart(2, '0');
  const name = `${SHOT_PREFIX}${stamp.getFullYear()}${pad(stamp.getMonth() + 1)}${pad(stamp.getDate())}`
        + `-${pad(stamp.getHours())}${pad(stamp.getMinutes())}${pad(stamp.getSeconds())}.png`;
  return SHOT_DIR + name;
}

/** 把 RGB565 小端像素写成 PNG。 */
export function writePng(path, width, height, pixels) {
  const raw = Buffer.alloc(height * (1 + width * 3));
  for (let y = 0; y < height; y++) {
    const rowBase = y * (1 + width * 3);
    raw[rowBase] = 0; // 过滤器：none
    const base = y * width * 2;
    for (let x = 0; x < width; x++) {
      const value = pixels[base + 2 * x] | (pixels[base + 2 * x + 1] << 8);
      const red = (((value >> 11) & 0x1f) * 255 / 31) | 0;
      const green = (((value >> 5) & 0x3f) * 255 / 63) | 0;
      const blue = ((value & 0x1f) * 255 / 31) | 0;
      const px = rowBase + 1 + x * 3;
      raw[px] = red;
      raw[px + 1] = green;
      raw[px + 2] = blue;
    }
  }

  const chunk = (tag, data) => {
    const body = Buffer.concat([Buffer.from(tag), data]);
    const head = Buffer.alloc(4);
    head.writeUInt32BE(data.length, 0);
    const crc = Buffer.alloc(4);
    crc.writeUInt32BE(crc32(body) >>> 0, 0);
    return Buffer.concat([head, body, crc]);
  };

  const header = Buffer.alloc(13);
  header.writeUInt32BE(width, 0);
  header.writeUInt32BE(height, 4);
  header[8] = 8; // 位深
  header[9] = 2; // 颜色类型：真彩
  header[10] = 0;
  header[11] = 0;
  header[12] = 0;

  const body = Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', header),
    chunk('IDAT', deflateSync(raw, { level: 6 })),
    chunk('IEND', Buffer.alloc(0)),
  ]);
  mkdirSync(path.substring(0, Math.max(path.lastIndexOf('\\'), path.lastIndexOf('/'))), { recursive: true });
  writeFileSync(path, body);
}

/** PNG 需要的 CRC-32（zlib 同多项式）。 */
function crc32(data) {
  let crc = 0xffffffff;
  for (const byte of data) {
    crc ^= byte;
    for (let i = 0; i < 8; i++) {
      crc = crc & 1 ? (crc >>> 1) ^ 0xedb88320 : crc >>> 1;
    }
  }
  return ~crc;
}

/** 按偏移拼一张实机截图；缺块或超时都不写文件。 */
export class ShotCollector {
  constructor() {
    this.width = 0;
    this.height = 0;
    this.format = 0;
    this.total = 0;
    this.received = 0;
    this.chunks = 0;
    this.buffer = null;
    this.deadline = 0.0;
    this.ready = false;
    this.error = '';
  }

  begin(now, timeoutS) {
    this.width = 0;
    this.height = 0;
    this.format = 0;
    this.total = 0;
    this.received = 0;
    this.chunks = 0;
    this.buffer = null;
    this.ready = false;
    this.error = '';
    this.deadline = now + timeoutS;
  }

  onInfo(payload) {
    const { width, height, format } = parseImageInfo(payload);
    this.width = width;
    this.height = height;
    this.format = format;
    this.total = width * height * 2;
    this.buffer = Buffer.alloc(this.total);
    this.received = 0;
    this.chunks = 0;
    if (format !== IMAGE_FORMAT_RGB565_LE) {
      this.error = `不认识的像素格式 0x${format.toString(16).padStart(2, '0')}`;
    }
  }

  onData(payload) {
    if (this.buffer === null || this.error) {
      return;
    }
    const { offset, data } = parseImageChunk(payload);
    if (offset + data.length > this.buffer.length) {
      this.error = `分块越界：偏移 ${offset} + ${data.length} 字节`;
      return;
    }
    data.copy(this.buffer, offset);
    this.received += data.length;
    this.chunks += 1;
  }

  /** 收尾帧：按声明字节数与实收字节数判定完整性，返回结论（空串 = 完整）。 */
  onEnd(payload) {
    if (this.buffer === null) {
      return '收到截图收尾帧，但没见过声明帧';
    }
    if (this.error) {
      return `截图失败：${this.error}`;
    }
    const declared = parseImageEnd(payload);
    if (declared !== this.total || this.received !== this.total) {
      return `截图不完整：收到 ${this.received}/${this.total} 字节（设备声明 ${declared}）`;
    }
    this.ready = true;
    return '';
  }
}
