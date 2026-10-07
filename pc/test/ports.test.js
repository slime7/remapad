// 串口枚举与打开失败的提示：link 里与设备无关的纯逻辑。
import { describe, expect, it } from 'vitest';

import { openHint, portSortKey, serialPortNames } from '../src/link/serial.js';
import { portSelection, portSummary } from '../src/gui-shared.js';

function winError(code, message) {
  const err = new Error(message);
  err.code = code;
  return err;
}

describe('serialPortNames', () => {
  it('sorts com ports by number', () => {
    const values = [['\\Device\\USBSER000', 'COM12'], ['\\Device\\BthModem0', 'COM5'], ['\\Device\\BthModem1', 'COM6']];
    expect(serialPortNames(values)).toEqual(['COM5', 'COM6', 'COM12']);
  });

  it('drops duplicates and foreign names', () => {
    const values = [['a', 'COM3'], ['b', 'COM3'], ['c', 'LPT1'], ['d', 'BTH1']];
    expect(serialPortNames(values)).toEqual(['COM3']);
  });

  it('no ports', () => {
    expect(serialPortNames([])).toEqual([]);
  });

  it('unknown port names sort after com ports', () => {
    expect(portSortKey('COM9')).toEqual(['', 9]);
    expect(portSortKey('PRN1')).toEqual(['PRN1', 0]);
  });
});

describe('openHint', () => {
  it('reports busy port', () => {
    expect(openHint(winError(32, '占用'))).toContain('占用');
    expect(openHint(winError(5, '拒绝访问'))).toContain('占用');
  });

  it('reports missing port', () => {
    expect(openHint(winError(2, '找不到'))).toContain('不存在');
  });

  it('reports other failures', () => {
    expect(openHint(winError(1, '其他'))).toBe('打开端口失败');
  });
});

describe('portSelection', () => {
  it('first port wins before the user picks anything', () => {
    expect(portSelection(['COM5', 'COM7'], 'COM3', 'COM3', false)).toBe('COM5');
  });

  it('keeps the user pick', () => {
    expect(portSelection(['COM5', 'COM7'], 'COM7', 'COM3', true)).toBe('COM7');
  });

  it('falls back when nothing is plugged in', () => {
    expect(portSelection([], 'COM7', 'COM3', true)).toBe('COM7');
    expect(portSelection([], '', 'COM3', false)).toBe('COM3');
  });

  it('leaves a cleared box empty', () => {
    expect(portSelection(['COM5'], '', 'COM3', true)).toBe('');
  });
});

describe('portSummary', () => {
  it('marks the selected port', () => {
    expect(portSummary(['COM3', 'COM5'], 'COM3')).toBe('串口 2 个：COM3（已选）  COM5');
  });

  it('says when the selection is missing', () => {
    expect(portSummary(['COM3'], 'COM9')).toContain('COM9 不在列表里');
  });

  it('no ports points at the refresh button', () => {
    const text = portSummary([], 'COM3');
    expect(text).toContain('没有检测到串口');
    expect(text).toContain('USB-Serial/JTAG');
  });
});
