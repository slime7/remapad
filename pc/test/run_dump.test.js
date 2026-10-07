// --dump 的选柄规则：应与转发一致地按 --vid/--pid 过滤，而不是固定抓第一只。
import { describe, expect, it, vi } from 'vitest';

import { runDump } from '../src/ctrl.js';

function fakeHid(devices) {
  return {
    devices,
    opened: null,
    enumerate() {
      return this.devices;
    },
    async open(path) {
      this.opened = path;
      return {
        read: async () => null,
        write: async () => 0,
        close: async () => {},
      };
    },
  };
}

function device({
  path = 'if0', vid = 0x054c, pid = 0x0ce6, usagePage = 0x01, usage = 0x05, product = 'Fake Pad',
} = {}) {
  return { path, vendorId: vid, productId: pid, usagePage, usage, product, interface: 0 };
}

function args({ vid = null, pid = null, padPath = null, seconds = 0.02 } = {}) {
  return { vid, pid, padPath, seconds };
}

describe('runDump', () => {
  it('opens the vid pid filtered interface', async () => {
    const hid = fakeHid([device({ path: 'ps' }), device({ path: 'xbox', vid: 0x045e, pid: 0x0b13 })]);
    const log = vi.spyOn(console, 'log').mockImplementation(() => {});
    const code = await runDump(args({ vid: 0x045e, pid: 0x0b13 }), hid);
    expect(code).toBe(0);
    expect(hid.opened).toBe('xbox');
    expect(log).toHaveBeenCalled();
  });

  it('keeps first interface without filter', async () => {
    const hid = fakeHid([device({ path: 'ps' }), device({ path: 'xbox', vid: 0x045e, pid: 0x0b13 })]);
    vi.spyOn(console, 'log').mockImplementation(() => {});
    const code = await runDump(args(), hid);
    expect(code).toBe(0);
    expect(hid.opened).toBe('ps');
  });

  it('fails with hint when filter matches nothing', async () => {
    const hid = fakeHid([device({ path: 'ps' })]);
    const err = vi.spyOn(console, 'error').mockImplementation(() => {});
    const code = await runDump(args({ vid: 0x1234 }), hid);
    expect(code).toBe(1);
    expect(err).toHaveBeenCalledWith('--vid/--pid 没有匹配到任何手柄接口（--list 可以看到全部候选）');
  });

  it('prints the chosen interface in dump line', async () => {
    const hid = fakeHid([
      device({ path: 'ps' }), device({ path: 'xbox', vid: 0x045e, pid: 0x0b13, product: 'Xbox Pad' }),
    ]);
    const log = vi.spyOn(console, 'log').mockImplementation(() => {});
    const code = await runDump(args({ vid: 0x045e }), hid);
    expect(code).toBe(0);
    expect(log.mock.calls.some((call) => String(call[0]).includes('Xbox Pad'))).toBe(true);
  });
});
