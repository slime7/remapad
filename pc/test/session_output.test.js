// 会话输出的分流：命令行写标准流、界面写队列，以及命令行的本行命令解析。
import { afterEach, describe, expect, it, vi } from 'vitest';

import { maskSecrets } from '../src/link/secrets.js';
import { ConsoleReporter, QueueReporter } from '../src/session/reporter.js';
import { ImageError } from '../src/session/image.js';
import { Session } from '../src/session/session.js';
import { DEFAULT_IMAGE, parseCtrlArgs } from '../src/args.js';

class FakeLink {
  constructor() {
    this.written = [];
    this.flushed = 0;
  }

  write(data) {
    this.written.push(Buffer.from(data));
  }

  flush() {
    this.flushed += 1;
  }

  purgeInput() {}
}

function makeSession(reporter = null) {
  const args = parseCtrlArgs([]);
  const fake = new FakeLink();
  return [new Session(args, null, fake, reporter), fake];
}

afterEach(() => {
  vi.restoreAllMocks();
});

describe('ConsoleReporter', () => {
  it('line goes to stdout and error to stderr', () => {
    const log = vi.spyOn(console, 'log').mockImplementation(() => {});
    const err = vi.spyOn(console, 'error').mockImplementation(() => {});
    const reporter = new ConsoleReporter();
    reporter.line('普通行');
    reporter.error('错误行');
    reporter.event('随便什么事件', { value: 1 }); // 命令行不关心事件
    expect(log).toHaveBeenCalledWith('普通行');
    expect(err).toHaveBeenCalledWith('错误行');
  });

  it('line masks wifi passwords', () => {
    const log = vi.spyOn(console, 'log').mockImplementation(() => {});
    const reporter = new ConsoleReporter();
    reporter.line('ok netlog cred ssid=slime_nest pass=hunter2');
    expect(log).toHaveBeenCalledWith('ok netlog cred ssid=slime_nest pass=***');
  });
});

describe('maskSecrets', () => {
  it('save command with echo prefix', () => {
    expect(maskSecrets('> netlog save slime_nest ssssssss')).toBe('> netlog save slime_nest ***');
  });

  it('direct connect command masks password keeps ip and port', () => {
    expect(maskSecrets('> netlog slime_nest hunter2 192.168.1.5 9999'))
      .toBe('> netlog slime_nest *** 192.168.1.5 9999');
    expect(maskSecrets('> netlog slime_nest hunter2')).toBe('> netlog slime_nest ***');
    // 没有回显前缀的行是设备侧输出而非发出的命令，不碰（固件日志不会回显命令）。
    expect(maskSecrets('netlog slime_nest hunter2 192.168.1.5 9999'))
      .toBe('netlog slime_nest hunter2 192.168.1.5 9999');
  });

  it('cred reply masks password keeps unset dash', () => {
    expect(maskSecrets('ok netlog cred ssid=slime_nest pass=hunter2'))
      .toBe('ok netlog cred ssid=slime_nest pass=***');
    expect(maskSecrets('ok netlog cred ssid=- pass=-')).toBe('ok netlog cred ssid=- pass=-');
  });

  it('subcommands status and usage lines stay untouched', () => {
    for (const line of ['netlog cred', 'netlog off', 'netlog scan',
      'netlog state=connected ssid=slime_nest dest=192.168.1.5:9999',
      'netlog session ssid=slime_nest port=9999 (dest learn)',
      'err usage: netlog [save <ssid> <password> | cred | scan | scanlist | off]',
      'ok wifi saved', 'ok netlog stopped']) {
      expect(maskSecrets(line)).toBe(line);
    }
  });
});

describe('QueueReporter', () => {
  it('records keep kind and fields', () => {
    const sink = [];
    const reporter = new QueueReporter(sink);
    reporter.line('普通行');
    reporter.error('错误行');
    reporter.event('shot_saved', { path: 'a.png', chunks: 3 });
    expect(sink[0]).toEqual({ kind: 'line', text: '普通行' });
    expect(sink[1]).toEqual({ kind: 'error', text: '错误行' });
    expect(sink[2]).toEqual({ kind: 'event', name: 'shot_saved', path: 'a.png', chunks: 3 });
  });

  it('session writes device lines into the queue', () => {
    const sink = [];
    const [session] = makeSession(new QueueReporter(sink));
    session.handleText(Buffer.from('ok key injected\r\n'));
    session.handleText(Buffer.from('state pairing=paired\n\n'));
    expect(sink.map((record) => record.text)).toEqual(['ok key injected', 'state pairing=paired']);
  });

  it('partial line waits for the newline', () => {
    const sink = [];
    const [session] = makeSession(new QueueReporter(sink));
    session.handleText(Buffer.from('ok half'));
    expect(sink).toHaveLength(0);
    session.handleText(Buffer.from(' line\n'));
    expect(sink[0].text).toBe('ok half line');
  });
});

describe('feedback write back', () => {
  function fakePad(written) {
    return {
      reports: [],
      async write(payload) {
        this.reports.push(Buffer.from(payload));
        if (written instanceof Error) {
          throw written;
        }
        return written;
      },
    };
  }

  function sessionWithPad(written) {
    const sink = [];
    const [session] = makeSession(new QueueReporter(sink));
    session.pad = fakePad(written);
    return [session, sink];
  }

  it('padded write counts as success', async () => {
    const [session, sink] = sessionWithPad(547);
    await expect(session.sendOutputReport(Buffer.alloc(78))).resolves.toBe(true);
    expect(session.writebackShort).toBe(0);
    expect(sink).toHaveLength(0);
  });

  it('short write is counted and reported', async () => {
    const [session, sink] = sessionWithPad(77);
    await expect(session.sendOutputReport(Buffer.alloc(78))).resolves.toBe(false);
    expect(session.writebackShort).toBe(1);
    expect(sink[0].text).toContain('短写');
  });
});

describe('local commands', () => {
  it('shot path keeps spaces', () => {
    const [session, fake] = makeSession();
    session.runLocal('shot C:\\my shots\\ui 1.png');
    expect(session.shotPath).toBe('C:\\my shots\\ui 1.png');
    expect(fake.written).toEqual([Buffer.from('shot\r')]);
  });

  it('default image path comes from the cli defaults', () => {
    const [session] = makeSession();
    expect(session.args.image).toBe(DEFAULT_IMAGE);
  });

  it('bad image path raises instead of exiting', () => {
    const [session] = makeSession();
    // 镜像不合法时抛异常：命令行映射成退出码 2，界面把它标红，都不许直接结束进程。
    expect(() => session.runLocal('ota Z:\\nope\\image.bin')).toThrow(ImageError);
  });

  it('help lists local commands', () => {
    const sink = [];
    const [session] = makeSession(new QueueReporter(sink));
    session.runLocal('help');
    const text = sink.map((record) => record.text).join('\n');
    expect(text).toContain('本工具命令');
    expect(text).toContain(':shot [路径]');
  });

  it('unknown local command reports error', () => {
    const sink = [];
    const [session] = makeSession(new QueueReporter(sink));
    session.runLocal('nope');
    expect(sink[0].kind).toBe('error');
  });

  it('quit stops the session', () => {
    const [session] = makeSession();
    session.runLocal('quit');
    expect(session.stop).toBe(true);
  });
});
