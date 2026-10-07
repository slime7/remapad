// 主机原始输出采集的 PC 侧纯逻辑：采集帧解析与落盘文件格式。
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { afterEach, describe, expect, it } from 'vitest';

import { FrameDecoder, TYPE_HOST_RAW, WIRE_MAX_PAYLOAD, encode, parseHostRaw } from '../src/link/frame.js';
import { HostCaptureSink } from '../src/session/capture.js';

function rumblePayload() {
  const data = Buffer.from([0x7c, 0x04, 0x80, 0x01, 0x97, 0x63, ...new Array(26).fill(0)]);
  return Buffer.concat([Buffer.from([0x12, data.length]), data]); // 0x12 = 震动输出通道
}

const tempDirs = [];
afterEach(() => {
  while (tempDirs.length) {
    rmSync(tempDirs.pop(), { recursive: true, force: true });
  }
});

describe('parseHostRaw', () => {
  it('rumble record round trip', () => {
    const parsed = parseHostRaw(rumblePayload());
    expect(parsed.channel).toBe(0x12);
    expect(parsed.name).toBe('rumble');
    expect(parsed.truncated).toBe(false);
    expect(parsed.data).toHaveLength(32);
    expect(parsed.data.subarray(0, 2)).toEqual(Buffer.from([0x7c, 0x04]));
  });

  it('truncation flag and unknown channel', () => {
    // 标志字节 bit7 = 截断，低 7 位 = 数据长度；未登记通道按 ch-<hex> 显示。
    const parsed = parseHostRaw(Buffer.from([0x31, 0x80 | 0x02, 0xaa, 0xbb]));
    expect(parsed.truncated).toBe(true);
    expect(parsed.data).toHaveLength(2);
    expect(parsed.name).toBe('ch-31');
  });

  it('payload shorter than header is rejected', () => {
    expect(() => parseHostRaw(Buffer.from([0x12]))).toThrow();
  });

  it('frame level round trip', () => {
    // 设备帧 → 解码 → 采集帧解析，整条链路对得上。
    const frame = encode(TYPE_HOST_RAW, 7, 0, rumblePayload(), WIRE_MAX_PAYLOAD);
    const { frames } = new FrameDecoder().feed(frame);
    expect(frames[0].type).toBe(TYPE_HOST_RAW);
    expect(frames[0].slot).toBe(7);
    expect(parseHostRaw(frames[0].payload).name).toBe('rumble');
  });
});

describe('HostCaptureSink', () => {
  it('records land as readable lines', () => {
    const dir = mkdtempSync(join(tmpdir(), 'remapad-'));
    tempDirs.push(dir);
    const path = join(dir, 'host-raw.log');
    const sink = new HostCaptureSink(path, 100.0);
    sink.open();
    sink.writeRecord(parseHostRaw(rumblePayload()), 0, 100.5);
    sink.writeRecord(parseHostRaw(Buffer.from([0x14, 0x01, 0x09])), 1, 101.25);
    const summary = sink.close();
    const text = readFileSync(path, 'utf8');
    expect(summary).toContain('2 条');
    const lines = text.trim().split('\n');
    expect(lines).toHaveLength(4); // 两行头注释 + 两条记录
    expect(lines[0].startsWith('# remapad host raw capture')).toBe(true);
    expect(lines[2]).toContain('+0.500s rumble[0x12] seq=000  32B');
    expect(lines[2]).toContain('7c 04 80 01 97 63');
    expect(lines[3]).toContain('+1.250s cmd[0x14] seq=001   1B 09');
  });

  it('seq gap is counted not fatal', () => {
    const dir = mkdtempSync(join(tmpdir(), 'remapad-'));
    tempDirs.push(dir);
    const sink = new HostCaptureSink(join(dir, 'host-raw.log'), 100.0);
    sink.open();
    sink.writeRecord(parseHostRaw(rumblePayload()), 0, 100.0);
    // 记录号跳到 5：设备侧队列满丢过包，继续往后写而不是中断。
    sink.writeRecord(parseHostRaw(rumblePayload()), 5, 100.1);
    sink.writeRecord(parseHostRaw(rumblePayload()), 6, 100.2);
    const summary = sink.close();
    expect(summary).toContain('1 处跳号');
    expect(summary).toContain('3 条');
  });

  it('close without open is null', () => {
    const dir = mkdtempSync(join(tmpdir(), 'remapad-'));
    tempDirs.push(dir);
    const sink = new HostCaptureSink(join(dir, 'host-raw.log'), 100.0);
    expect(sink.close()).toBeNull();
  });
});
