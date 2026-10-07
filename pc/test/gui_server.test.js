// GUI 后端纯逻辑：命令路由与守卫、回读吸收、连接/升级守卫、会话生命周期与事件状态机。
import { describe, expect, test, vi } from 'vitest';
import { Buffer } from 'node:buffer';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { mkdtempSync, writeFileSync } from 'node:fs';

import {
  APP_DESC_FIELD_LEN,
  APP_DESC_MAGIC,
  APP_DESC_OFFSET,
  APP_DESC_PROJECT_OFFSET,
  APP_DESC_VERSION_OFFSET,
  ESP_CHIP_ID_OFFSET,
  ESP_IMAGE_MAGIC,
  loadImage,
} from '../src/session/image.js';
import { GuiServer } from '../src/gui-server/server.js';
import { HidUnavailable } from '../src/input/pads.js';

/** 够用的会话替身：runInteractive 挂在 gate 上，测试决定收尾时刻与退出码。 */
class FakeSession {
  constructor(args, hid, conn, reporter) {
    this.args = args;
    this.hid = hid;
    this.conn = conn;
    this.reporter = reporter;
    this.commands = [];
    this.stop = false;
    this.stopCode = 0;
    this.padInfo = null;
    this.reports = 3;
    this.frames = 5;
    this.outputs = 7;
    this.forward = true;
    this.gate = new Promise((resolve) => { this.release = resolve; });
  }

  async runInteractive() {
    await this.gate;
    return this.stopCode;
  }

  async detachPad() {
    this.detached = true;
  }
}

function makeGui(overrides = {}) {
  const records = [];
  const gui = new GuiServer({
    record: (record) => records.push(record),
    sessionClass: FakeSession,
    createSerial: () => ({ close: vi.fn() }),
    ...overrides,
  });
  return { gui, records };
}

function lastText(records, kind = 'error') {
  const matched = records.filter((record) => record.kind === kind);
  return matched.length ? matched[matched.length - 1].text : null;
}

/** 走真实 connectSerial 路径建一个假会话；net 给了就走 connectNet。 */
async function startSession(gui, { net = null, port = 'COM12' } = {}) {
  if (net !== null) {
    await gui.connectNet(net);
  } else {
    await gui.connectSerial(port);
  }
  expect(gui.session).toBeInstanceOf(FakeSession);
  return gui.session;
}

/** 让假会话收尾：等 finally 链把引用清干净。 */
async function finishSession(gui, code = 0) {
  const session = gui.session;
  session.stopCode = code;
  session.release();
  await vi.waitFor(() => expect(gui.session).toBeNull());
}

/** 与固件 app 镜像同构的最小镜像：魔法数、芯片标识与应用描述符齐全。 */
function makeImageBytes(version = '9.9.9') {
  const data = Buffer.alloc(APP_DESC_PROJECT_OFFSET + APP_DESC_FIELD_LEN);
  data[0] = ESP_IMAGE_MAGIC;
  data.writeUInt16LE(0x0009, ESP_CHIP_ID_OFFSET);
  data.writeUInt32LE(APP_DESC_MAGIC, APP_DESC_OFFSET);
  data.write(`${version}\0`, APP_DESC_VERSION_OFFSET, 'latin1');
  data.write('remapad_firmware\0', APP_DESC_PROJECT_OFFSET, 'latin1');
  return data;
}

describe('命令路由与守卫', () => {
  test('未连接时命令不发出', () => {
    const { gui, records } = makeGui();
    gui.sendCommand('status');
    expect(lastText(records)).toBe('未连接设备，命令没有发出');
  });

  test('空命令被忽略', () => {
    const { gui, records } = makeGui();
    gui.sendCommand('   ');
    expect(records).toHaveLength(0);
  });

  test('网络会话拒绝截图，普通命令照常进队列', async () => {
    const close = vi.fn();
    const { gui, records } = makeGui({
      createUdp: () => ({ close, address: ['192.168.1.5', 9999] }),
    });
    await startSession(gui, { net: '192.168.1.5:9999' });
    expect(gui.snapshot().netSession).toBe(true);
    gui.sendCommand(':shot');
    expect(lastText(records)).toContain('截图只走串口');
    gui.sendCommand('status');
    expect(lastText(records, 'send')).toBe('> status');
    expect(gui.session.commands).toContain('status');
    await finishSession(gui);
  });

  test('命令回显脱敏：netlog save 的密码不打进日志', async () => {
    const { gui, records } = makeGui();
    await startSession(gui);
    gui.sendCommand('netlog save slime_nest hunter2');
    expect(lastText(records, 'send')).toBe('> netlog save slime_nest ***');
    await finishSession(gui);
  });

  test('连上设备自动排一遍设置回读命令', async () => {
    const { gui } = makeGui();
    const session = await startSession(gui);
    for (const command of ['status', 'version', 'ctrl', 'ds', 'netlog', 'netlog cred']) {
      expect(session.commands).toContain(command);
    }
    await finishSession(gui);
  });
});

describe('连接守卫', () => {
  test('没有串口时提示先选择', async () => {
    const { gui, records } = makeGui();
    await gui.connectSerial('');
    expect(lastText(records)).toBe('先选一个串口再连接');
    expect(gui.snapshot().sessionState).toBe('disconnected');
  });

  test('打开失败转成错误提示，不留在连接中', async () => {
    const { gui, records } = makeGui({
      createSerial: () => {
        const exc = new Error('拒绝访问。');
        exc.code = 'EACCES';
        throw exc;
      },
    });
    await gui.connectSerial('COM404');
    expect(lastText(records)).toContain('COM404');
    expect(gui.snapshot().sessionState).toBe('disconnected');
  });

  test('已有会话时再连被拒绝', async () => {
    const { gui, records } = makeGui();
    await startSession(gui);
    await gui.connectSerial('COM9');
    expect(lastText(records)).toBe('已有会话在跑，先断开再连');
    await finishSession(gui);
  });

  test('网络目标缺省与非法都被拦下', async () => {
    const { gui, records } = makeGui();
    await gui.connectNet('');
    expect(lastText(records)).toContain('先填 设备IP:端口');
    await gui.connectNet(':9999');
    expect(lastText(records)).toContain('地址里没有 IP');
    expect(gui.snapshot().connected).toBe(false);
  });

  test('串口连接成功后端口选择被记住', async () => {
    const { gui } = makeGui();
    await startSession(gui, { port: 'COM12' });
    expect(gui.port).toBe('COM12');
    expect(gui.portChosen).toBe(true);
    expect(gui.snapshot().connected).toBe(true);
    await finishSession(gui);
  });

  test('断开请求置 stop 标记，由会话循环自行收尾', async () => {
    const { gui } = makeGui();
    const session = await startSession(gui);
    gui.disconnect();
    expect(session.stop).toBe(true);
    await finishSession(gui);
  });
});

describe('回读吸收：设备是唯一事实源', () => {
  test('配色回读刷新四段与摘要', () => {
    const { gui } = makeGui();
    gui.absorbReply('ok ctrl body=0x112233 button=0x445566 accent=0x778899 grip=0xaabbcc');
    expect(gui.settings.colors).toEqual([0x112233, 0x445566, 0x778899, 0xaabbcc]);
    expect(gui.settings.colorSummary).toContain('0x112233');
  });

  test('亮度、息屏与 DS 行为回读各归各位', () => {
    const { gui } = makeGui();
    gui.absorbReply('backlight 42');
    gui.absorbReply('screen off');
    gui.absorbReply('ok ds touchpad=on capture=off');
    expect(gui.settings.brightness).toBe(42);
    expect(gui.settings.screenOn).toBe(false);
    expect(gui.settings.dsTouchpad).toBe(true);
    expect(gui.settings.dsCapture).toBe(false);
  });

  test('WiFi 凭据回读进展示态，日志里的密码被掩掉', async () => {
    const { gui, records } = makeGui();
    await startSession(gui);
    gui.makeReporter().line('ok netlog cred ssid=slime_nest pass=hunter2');
    expect(gui.settings.wifiSsid).toBe('slime_nest');
    expect(gui.settings.wifiPass).toBe('hunter2');
    expect(lastText(records, 'line')).toBe('ok netlog cred ssid=slime_nest pass=***');
    await finishSession(gui);
  });

  test('设备事实回读合并并拼展示行', () => {
    const { gui } = makeGui();
    gui.makeReporter().line('state fw=1.2.3 part=ota_0 pairing=paired uptime=90s heap=10240/65536');
    expect(gui.facts.firmware).toBe('1.2.3');
    expect(gui.facts.uptime_s).toBe(90);
    const text = gui.snapshot().factsText;
    expect(text).toContain('固件 1.2.3');
    expect(text).toContain('运行 01:30');
    expect(text).toContain('堆内存 54 / 64 KB');
    expect(text).not.toContain('电量');
  });

  test('认不出的行不碰展示态', () => {
    const { gui } = makeGui();
    gui.absorbReply('ok 已转发 3 帧报告');
    expect(gui.settings.colorSummary).toBe('当前：未读取');
  });
});

describe('事件状态机', () => {
  test('升级进度与完成刷新展示态', () => {
    const { gui } = makeGui();
    gui.makeReporter().event('ota_progress', { total: 200, confirmed: 100 });
    expect(gui.snapshot().ota).toEqual({ percent: 0.5, label: '写入 100 / 200 字节（50%）' });
    gui.otaWait = true;
    gui.makeReporter().event('ota_finished', { ok: true });
    expect(gui.pendingOtaWait).toBe(true);
    expect(gui.snapshot().ota).toEqual({ percent: 0, label: '升级完成' });
  });

  test('串口升级完成后走等设备回来的流程并自动重连', async () => {
    const waits = [];
    const { gui } = makeGui({
      waitForVersion: (port) => {
        waits.push(port);
        return Promise.resolve(0);
      },
    });
    const oldSession = await startSession(gui, { port: 'COM12' });
    gui.otaWait = true;
    gui.makeReporter().event('ota_finished', { ok: true });
    oldSession.stopCode = 1;
    oldSession.release();
    await vi.waitFor(() => expect(waits).toEqual(['COM12']));
    // device_back 自动重连：新会话顶上，不再是收尾的那一个。
    await vi.waitFor(() => expect(gui.snapshot().connected).toBe(true));
    expect(gui.session).not.toBe(oldSession);
    expect(gui.port).toBe('COM12');
  });

  test('会话收尾清理引用并标记断开', async () => {
    const close = vi.fn();
    const { gui } = makeGui({ createSerial: () => ({ close }) });
    await startSession(gui);
    await finishSession(gui);
    expect(close).toHaveBeenCalled();
    expect(gui.snapshot().connected).toBe(false);
    expect(gui.snapshot().sessionState).toBe('disconnected');
  });

  test('异常收尾标成链路断开并提示', async () => {
    const { gui, records } = makeGui();
    await startSession(gui);
    await finishSession(gui, 1);
    expect(gui.snapshot().sessionState).toBe('broken');
    expect(lastText(records)).toContain('链路已断开');
  });

  test('截图事件调用系统打开器', () => {
    const opened = [];
    const { gui } = makeGui({ openPath: (p) => opened.push(p) });
    gui.makeReporter().event('shot_saved', { path: 'C:/shots/x.png' });
    expect(opened).toEqual(['C:/shots/x.png']);
    const line = gui.recent.filter((record) => record.kind === 'line').pop();
    expect(line.text).toContain('截图已落盘');
  });
});

describe('升级守卫', () => {
  test('未连接时拒绝升级', () => {
    const { gui, records } = makeGui();
    expect(gui.startUpgrade('x.bin', true).ok).toBe(false);
    expect(lastText(records)).toBe('先连接设备再升级');
  });

  test('镜像校验通过与失败两条路', () => {
    const dir = mkdtempSync(join(tmpdir(), 'remapad-gui-'));
    const good = join(dir, 'good.bin');
    writeFileSync(good, makeImageBytes('9.9.9'));
    const { gui, records } = makeGui();
    const ok = gui.validateImage(good);
    expect(ok).toMatchObject({ ok: true, bytes: makeImageBytes().length, version: '9.9.9' });
    expect(loadImage(good).version).toBe('9.9.9');
    const bad = gui.validateImage(join(dir, 'missing.bin'));
    expect(bad.ok).toBe(false);
    expect(lastText(records)).toContain('读不到镜像');
    expect(gui.snapshot().ota.label).toBe('镜像不合法');
  });
});

describe('端口与手柄刷新', () => {
  test('refreshPorts 落在本机第一个口并写日志', async () => {
    const { gui, records } = makeGui({ listPorts: async () => ['COM5', 'COM6'] });
    await gui.refreshPorts();
    expect(gui.port).toBe('COM5');
    expect(lastText(records, 'line')).toContain('串口 2 个');
  });

  test('refreshPads 记录候选并进快照，选择按索引生效', async () => {
    const infos = [
      {
        path: 'p1', usagePage: 0x01, usage: 0x04, vendorId: 0x054c, productId: 0x05c4,
        product: 'DualSense', interface: 0, busType: 'usb',
      },
      {
        path: 'p2', usagePage: 0x01, usage: 0x05, vendorId: 0x054c, productId: 0x05c4,
        product: 'DualSense', interface: 2, busType: 'usb',
      },
    ];
    const { gui } = makeGui({ loadHid: () => ({ enumerate: () => infos }) });
    await gui.refreshPads();
    expect(gui.padEntries).toHaveLength(2);
    expect(gui.snapshot().pads).toHaveLength(2);
    gui.selectPad(1);
    expect(gui.selectedPadPath()).toBe('p2');
    gui.selectPad(null);
    expect(gui.selectedPadPath()).toBeNull();
  });

  test('hidapi 不可用时降级并提示，其他异常向上抛', async () => {
    const { gui, records } = makeGui({
      loadHid: () => {
        throw new Error('node-hid 崩了');
      },
    });
    await expect(gui.refreshPads()).rejects.toThrow('node-hid 崩了');
    const { gui: gui2, records: records2 } = makeGui({
      loadHid: () => {
        throw new HidUnavailable('缺少 node-hid：在仓库根执行 pnpm install 后重试');
      },
    });
    await gui2.refreshPads();
    expect(gui2.hid).toBeNull();
    expect(lastText(records2)).toContain('缺少 node-hid');
    expect(gui2.snapshot().hidReady).toBe(false);
    void records;
  });
});

describe('转发开关与 WiFi 保存', () => {
  test('未连接时转发开关只提示', () => {
    const { gui, records } = makeGui();
    gui.setForward(false);
    expect(lastText(records, 'line')).toContain('连接后转发开关才会生效');
  });

  test('已连接时落到会话上', async () => {
    const { gui, records } = makeGui();
    await startSession(gui);
    gui.setForward(false);
    expect(gui.session.forward).toBe(false);
    const lines = records.filter((record) => record.kind === 'line').map((record) => record.text);
    expect(lines.some((text) => text.includes('手柄转发已关闭'))).toBe(true);
    await finishSession(gui);
  });

  test('WiFi 保存的校验：两项都填且不带空格', async () => {
    const { gui, records } = makeGui();
    await startSession(gui);
    gui.saveWifi('', '');
    expect(lastText(records)).toBe('SSID 与密码都要填');
    gui.saveWifi('a b', 'c');
    expect(lastText(records)).toBe('SSID 与密码暂不支持空格');
    gui.saveWifi('slime_nest', 'hunter2');
    expect(gui.session.commands).toContain('netlog save slime_nest hunter2');
    await finishSession(gui);
  });
});
