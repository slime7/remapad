// 日志脱敏：只隐 WiFi 密码，SSID 与会话状态保持可读。
import { describe, expect, it } from 'vitest';

import { maskSecrets } from '../src/link/secrets.js';

describe('maskSecrets', () => {
  it('masks the password in credential readbacks', () => {
    expect(maskSecrets('ok netlog cred ssid=slime_nest pass=hunter2'))
      .toBe('ok netlog cred ssid=slime_nest pass=***');
  });

  it('keeps the unconfigured dash as is', () => {
    expect(maskSecrets('ok netlog cred ssid=- pass=-'))
      .toBe('ok netlog cred ssid=- pass=-');
  });

  it('masks the save command echo', () => {
    expect(maskSecrets('> netlog save slime_nest hunter2'))
      .toBe('> netlog save slime_nest ***');
  });

  it('masks the connect command echo', () => {
    expect(maskSecrets('> netlog slime_nest hunter2'))
      .toBe('> netlog slime_nest ***');
  });

  it('does not mistake netlog log words for a command', () => {
    expect(maskSecrets('netlog save flag set to 1')).toBe('netlog save flag set to 1');
    expect(maskSecrets('netlog state=connected ssid=slime_nest dest=192.168.1.5:9999'))
      .toBe('netlog state=connected ssid=slime_nest dest=192.168.1.5:9999');
  });
});
