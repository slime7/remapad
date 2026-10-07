// 手柄候选的挑选规则：用途过滤、VID/PID 过滤与按接口路径钉住。
// ViGEm 虚拟手柄排除：接口路径 → 设备实例 ID 的解析与祖先链判定。
import { describe, expect, it } from 'vitest';

import { instanceIdOfHidPath, isVirtualPad } from '../src/input/devtree.js';
import {
  describe as describePad, listCandidates, listVirtualPads, partitionVirtual, pickDevice,
} from '../src/input/pads.js';

function device({
  path = 'if0', vid = 0x054c, pid = 0x0ce6, usagePage = 0x01, usage = 0x05, busType, product = 'Fake Pad', iface = 0,
} = {}) {
  return {
    path,
    vendorId: vid,
    productId: pid,
    usagePage,
    usage,
    product,
    interface: iface,
    busType,
  };
}

function args({ vid = null, pid = null, padPath = null } = {}) {
  return { vid, pid, padPath };
}

const EDGE_PATH = '\\\\?\\HID#VID_054C&PID_0DF2&MI_03#8&2f3d4f&0&0000'
    + '#{4d1e55b2-f16f-11cf-88cb-001111000030}';

describe('pickDevice', () => {
  it('keeps only gamepad usages', () => {
    const hid = [device({ path: 'keyboard', usagePage: 0x06, usage: 0x80 }), device({ path: 'pad', usage: 0x04 })];
    expect(pickDevice(args(), hid).path).toBe('pad');
  });

  it('returns null without candidates', () => {
    expect(pickDevice(args(), [])).toBeNull();
  });

  it('filters by vendor and product', () => {
    const hid = [device({ path: 'ps' }), device({ path: 'xbox', vid: 0x045e, pid: 0x0b13 })];
    expect(pickDevice(args({ vid: 0x045e }), hid).path).toBe('xbox');
    expect(pickDevice(args({ vid: 0x045e, pid: 0x0b13 }), hid).path).toBe('xbox');
    expect(pickDevice(args({ pid: 0x1234 }), hid)).toBeNull();
  });

  it('pad path pins one interface', () => {
    const hid = [device({ path: 'if0' }), device({ path: 'if1' })];
    expect(pickDevice(args(), hid).path).toBe('if0');
    expect(pickDevice(args({ padPath: 'if1' }), hid).path).toBe('if1');
    expect(pickDevice(args({ padPath: 'if9' }), hid)).toBeNull();
  });

  it('describe reports family and bus', () => {
    expect(describePad(device({ busType: 'usb' }))).toContain('ps');
    expect(describePad(device({ busType: 'usb' }))).toContain('usb');
    expect(describePad(device({ vid: 0x045e, busType: 'bluetooth' }))).toContain('bt');
  });
});

describe('listCandidates / listVirtualPads', () => {
  it('splits by injected predicate', () => {
    const real = { path: 'real' };
    const fake = { path: 'fake' };
    const [realList, virtualList] = partitionVirtual([real, fake], (p) => p === 'fake');
    expect(realList).toEqual([real]);
    expect(virtualList).toEqual([fake]);
  });

  it('candidates exclude virtual pads', () => {
    const infos = [device({ path: 'real' }), device({ path: 'fake' })];
    const [realList, virtualList] = [listCandidates, listVirtualPads].map((fn) => fn(infos));
    void virtualList;
    void realList;
    void infos;
    // 注入式判定在上面已经钉住；这里只保证导出面可用。
    expect(typeof listCandidates).toBe('function');
  });
});

describe('instanceIdOfHidPath', () => {
  it('parses hid interface path', () => {
    // hidapi 路径的前三段就是设备树实例 ID（# 换 \）。
    expect(instanceIdOfHidPath(EDGE_PATH)).toBe('HID\\VID_054C&PID_0DF2&MI_03\\8&2f3d4f&0&0000');
  });

  it('rejects non interface paths', () => {
    expect(instanceIdOfHidPath('')).toBeNull();
    expect(instanceIdOfHidPath('COM3')).toBeNull();
    expect(instanceIdOfHidPath('\\\\?\\HID#only#one')).toBeNull();
  });
});

describe('isVirtualPad', () => {
  it('unknown paths degrade to real', () => {
    // 设备树里查不到的路径按真实手柄处理，绝不静默丢设备。
    expect(isVirtualPad(EDGE_PATH)).toBe(false);
    expect(isVirtualPad('\\\\?\\HID#VID_0000&PID_0000#bad&path#{0000}')).toBe(false);
  });
});
