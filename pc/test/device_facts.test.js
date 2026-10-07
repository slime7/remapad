// 设备信息行的拼装：界面这一行是设备屏幕「系统信息」页的镜像，口径要跟固件一致。
import { describe, expect, it } from 'vitest';

import { formatDeviceFacts } from '../src/gui-shared.js';

describe('formatDeviceFacts', () => {
  it('heap uses the device screen used over total', () => {
    expect(formatDeviceFacts({ heap: 88064, heap_total: 327680 })).toBe('堆内存 234 / 320 KB');
  });

  it('heap without total falls back to free', () => {
    expect(formatDeviceFacts({ heap: 88064 })).toBe('堆内存 空闲 86 KB');
  });

  it('full line keeps the existing segments', () => {
    const text = formatDeviceFacts({
      firmware: 'e04fd8c', partition: 'ota_0', image: 'confirmed',
      ota_state: 'idle', battery_mv: 4160, battery_percent: 100,
      uptime_s: 9153, pairing: 'paired', role: 'device',
    });
    expect(text.split('\n')).toEqual([
      '固件 e04fd8c ｜ 分区 ota_0 ｜ 镜像 已确认 ｜ 升级 idle',
      '运行 2:32:33 ｜ 配对 已配对 ｜ 角色 串口',
    ]);
  });

  it('battery facts are read but not displayed', () => {
    // 电量与端电压只留在事实数据里：设备已放弃自带电源管理，不上屏。
    expect(formatDeviceFacts({ battery_mv: 4160, battery_percent: 100, charging: true }))
      .toBe('设备没有回可读的状态');
  });

  it('empty facts says so', () => {
    expect(formatDeviceFacts({})).toBe('设备没有回可读的状态');
  });
});
