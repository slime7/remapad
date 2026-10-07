// 升级镜像与 amiibo dump 的本地校验：拒绝条件与提示文案。
// 数据与核对状态见 docs/ABSTRACTIONS.md。

import { readFileSync } from 'node:fs';

import { AMIIBO_NAME_MAX, AMIIBO_SIG_SIZE, AMIIBO_TAG_SIZE } from '../link/frame.js';

// --- OTA：镜像校验常量（与固件 ota/ 的约定一致）---
export const ESP_IMAGE_MAGIC = 0xe9;
export const ESP_CHIP_ID_OFFSET = 0x0c;
export const ESP_CHIP_ID_ESP32S3 = 0x0009;
export const APP_DESC_OFFSET = 0x20;
export const APP_DESC_MAGIC = 0xabcd5432;
export const APP_DESC_VERSION_OFFSET = APP_DESC_OFFSET + 0x10;
export const APP_DESC_PROJECT_OFFSET = APP_DESC_OFFSET + 0x30;
export const APP_DESC_FIELD_LEN = 32;
export const EXPECTED_PROJECT = 'remapad_firmware';
export const PARTITION_MAX_BYTES = 4 * 1024 * 1024;

/** 应用镜像不合法：内容、芯片标识或项目名不符合本设备的升级条件。 */
export class ImageError extends Error {}

/** amiibo 文件不合法：不是 540 字节的 NTAG215 dump，或拿不出可用的名称。 */
export class AmiiboError extends Error {}

/** 读入并校验应用镜像，返回 {data, version}；不合法抛 ImageError。 */
export function loadImage(path) {
  let data;
  try {
    data = readFileSync(path);
  } catch (exc) {
    throw new ImageError(`读不到镜像 ${path}：${exc.message}`);
  }
  if (data.length < APP_DESC_PROJECT_OFFSET + APP_DESC_FIELD_LEN) {
    throw new ImageError(`${path} 只有 ${data.length} 字节，不是应用镜像`);
  }
  if (data[0] !== ESP_IMAGE_MAGIC) {
    throw new ImageError(`${path} 首字节是 0x${data[0].toString(16).padStart(2, '0')}，不是 ESP-IDF 应用镜像`);
  }
  const chip = data.readUInt16LE(ESP_CHIP_ID_OFFSET);
  if (chip !== ESP_CHIP_ID_ESP32S3) {
    throw new ImageError(`${path} 的芯片标识是 0x${chip.toString(16).padStart(4, '0')}，不是 ESP32-S3`);
  }
  const magic = data.readUInt32LE(APP_DESC_OFFSET);
  if (magic !== APP_DESC_MAGIC) {
    throw new ImageError(`${path} 缺少应用描述符（magic 0x${magic.toString(16).padStart(8, '0')}）`);
  }
  const version = descField(data, APP_DESC_VERSION_OFFSET);
  const project = descField(data, APP_DESC_PROJECT_OFFSET);
  if (project !== EXPECTED_PROJECT) {
    throw new ImageError(`${path} 是 ${project || '未知'} 的镜像，本设备只接受 ${EXPECTED_PROJECT}`);
  }
  if (data.length > PARTITION_MAX_BYTES) {
    throw new ImageError(`${path} 有 ${data.length} 字节，超过应用分区容量 ${PARTITION_MAX_BYTES}`);
  }
  return { data, version };
}

export function descField(data, offset) {
  const raw = data.subarray(offset, offset + APP_DESC_FIELD_LEN);
  const end = raw.indexOf(0);
  return raw.subarray(0, end < 0 ? APP_DESC_FIELD_LEN : end).toString('utf8');
}

/** 读入 amiibo dump（540 字节 NTAG215 镜像，或 572 字节镜像 + 厂商签名），
 * 返回 {name, data}。572 的签名段进设备读缓冲头区，主机校验签名时必需。
 *
 * 名称取文件名主干，按 UTF-8 截到 31 字节（不在多字节字符中间截断），
 * 是设备侧槽位的显示名；不合法抛 AmiiboError。
 */
export function loadAmiibo(path) {
  let data;
  try {
    data = readFileSync(path);
  } catch (exc) {
    throw new AmiiboError(`读不到 amiibo 文件 ${path}：${exc.message}`);
  }
  if (data.length !== AMIIBO_TAG_SIZE && data.length !== AMIIBO_TAG_SIZE + AMIIBO_SIG_SIZE) {
    throw new AmiiboError(
      `${path} 是 ${data.length} 字节，amiibo dump 固定是 ${AMIIBO_TAG_SIZE}（纯镜像）`
            + `或 ${AMIIBO_TAG_SIZE + AMIIBO_SIG_SIZE}（镜像 + 厂商签名）字节`);
  }
  const stem = path.replace(/\\/g, '/').split('/').pop().replace(/\.[^.]+$/, '');
  const name = truncateUtf8(stem, AMIIBO_NAME_MAX).trim();
  if (!name) {
    throw new AmiiboError(`${path} 的文件名拿不出可用的槽位名`);
  }
  return { name, data };
}

/** 按 UTF-8 字节数截断，不在多字节字符中间留半个字（截半的尾部字节丢弃）。 */
export function truncateUtf8(text, maxBytes) {
  const buf = Buffer.from(text, 'utf8');
  if (buf.length <= maxBytes) {
    return text;
  }
  // Node 把截半的多字节尾部解成 U+FFFD，截断只会伤到结尾，直接剥掉。
  return buf.subarray(0, maxBytes).toString('utf8').replace(/\ufffd/g, '');
}
