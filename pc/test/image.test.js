// 升级镜像的本地校验：拒绝条件与提示文案。
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { afterEach, describe, expect, it } from 'vitest';

import {
  APP_DESC_MAGIC,
  APP_DESC_OFFSET,
  APP_DESC_PROJECT_OFFSET,
  APP_DESC_VERSION_OFFSET,
  ESP_CHIP_ID_OFFSET,
  PARTITION_MAX_BYTES,
  loadImage,
} from '../src/session/image.js';

function fakeImage({
  version = '1.2.3', project = 'remapad_firmware', size = 0, firstByte = 0xe9,
  chip = 0x0009, descMagic = APP_DESC_MAGIC,
} = {}) {
  const data = Buffer.alloc(size || APP_DESC_PROJECT_OFFSET + 64);
  data[0] = firstByte;
  data.writeUInt16LE(chip, ESP_CHIP_ID_OFFSET);
  data.writeUInt32LE(descMagic, APP_DESC_OFFSET);
  Buffer.from(version, 'utf8').copy(data, APP_DESC_VERSION_OFFSET);
  Buffer.from(project, 'utf8').copy(data, APP_DESC_PROJECT_OFFSET);
  return data;
}

const tempDirs = [];
afterEach(() => {
  while (tempDirs.length) {
    rmSync(tempDirs.pop(), { recursive: true, force: true });
  }
});

function tempFile(name, data) {
  const dir = mkdtempSync(join(tmpdir(), 'remapad-'));
  tempDirs.push(dir);
  const path = join(dir, name);
  writeFileSync(path, data);
  return path;
}

describe('loadImage', () => {
  it('accepts valid image and reads version', () => {
    const path = tempFile('image.bin', fakeImage({ version: '52b8bad-dirty' }));
    const { data, version } = loadImage(path);
    expect(version).toBe('52b8bad-dirty');
    expect(data).toHaveLength(APP_DESC_PROJECT_OFFSET + 64);
  });

  it('rejects missing file', () => {
    const dir = mkdtempSync(join(tmpdir(), 'remapad-'));
    tempDirs.push(dir);
    expect(() => loadImage(join(dir, 'nope.bin'))).toThrow(/读不到镜像/);
  });

  it('rejects truncated file', () => {
    const path = tempFile('image.bin', Buffer.from([0xe9, 0x00, 0x00]));
    expect(() => loadImage(path)).toThrow(/不是应用镜像/);
  });

  it('rejects foreign first byte', () => {
    const path = tempFile('image.bin', fakeImage({ firstByte: 0x42 }));
    expect(() => loadImage(path)).toThrow(/首字节/);
  });

  it('rejects other chip', () => {
    const path = tempFile('image.bin', fakeImage({ chip: 0x0002 }));
    expect(() => loadImage(path)).toThrow(/不是 ESP32-S3/);
  });

  it('rejects missing app descriptor', () => {
    const path = tempFile('image.bin', fakeImage({ descMagic: 0x12345678 }));
    expect(() => loadImage(path)).toThrow(/缺少应用描述符/);
  });

  it('rejects other project', () => {
    const path = tempFile('image.bin', fakeImage({ project: 'other_app' }));
    expect(() => loadImage(path)).toThrow(/本设备只接受 remapad_firmware/);
  });

  it('rejects image over partition size', () => {
    const path = tempFile('image.bin', fakeImage({ size: PARTITION_MAX_BYTES + 1 }));
    expect(() => loadImage(path)).toThrow(/超过应用分区容量/);
  });
});
