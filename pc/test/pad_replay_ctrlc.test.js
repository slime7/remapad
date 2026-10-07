// pad_replay.mjs 的 Ctrl-C 收尾：回放中打断要干净退出（退出码 130），
// 音频流/HID 句柄的 finally 照常走到，不把中断裸抛给用户。
import { describe, expect, it, vi } from 'vitest';

import { Interrupt, main } from './samples/pad_replay.mjs';

describe('ctrl-c exit', () => {
  it('interrupted replay exits cleanly', async () => {
    const closed = [];
    const fakeDev = { close: () => closed.push(true) };
    const saved = { ...globalThis };
    void saved;
    // 依赖注入点替换成中断场景：runBtHid 一进来就抛中断。
    const { deps } = await import('./samples/pad_replay.mjs');
    const savedDeps = { ...deps };
    try {
      deps.findPad = (conn) => (conn === 'bt' ? {} : null);
      deps.openHid = () => fakeDev;
      deps.runBtHid = () => {
        throw new Interrupt();
      };
      const log = vi.spyOn(console, 'log').mockImplementation(() => {});
      const code = await main(['ns2-search-page.capture', '--pad', 'bt']);
      const out = log.mock.calls.map((call) => call.join(' ')).join('\n');
      expect(code).toBe(130);
      expect(out).toContain('已中断');
      expect(closed).toEqual([true]);
    } finally {
      Object.assign(deps, savedDeps);
      vi.restoreAllMocks();
    }
  });
});
