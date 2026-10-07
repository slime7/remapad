// 设置回读行的解析：界面控件跟着固件回读走，认错一行就会显示错的当前值。
import { describe, expect, it } from 'vitest';

import { parseDeviceReply } from '../src/session/device-reply.js';
import { cssColor, formatUptime, parseColor, readableOn } from '../src/gui-shared.js';

// 固件 cli_status 的一行回读：pad 字段带空格（输入设备的型号描述）。
const STATUS_LINE = 'state pairing=idle role=device backlight=60 screen=1 uptime=1234s '
    + 'heap=458752/524288 batt=3971mV/85% chg=0 fw=v0.4.0-12-gabcdef part=ota_0 '
    + 'ota=idle ui=off pad=DualSense Edge (USB)';

describe('parseDeviceReply', () => {
  it('backlight reply carries percent', () => {
    expect(parseDeviceReply('backlight 60')).toEqual({ channel: 'backlight', fields: { light: 60 } });
  });

  it('backlight reply rejects junk', () => {
    for (const line of ['err backlight 0-100', 'backlight 101', 'backlight']) {
      expect(parseDeviceReply(line)).toBeNull();
    }
  });

  it('screen reply maps on and off', () => {
    expect(parseDeviceReply('screen on')).toEqual({ channel: 'screen', fields: { screen_on: true } });
    expect(parseDeviceReply('screen off')).toEqual({ channel: 'screen', fields: { screen_on: false } });
    expect(parseDeviceReply('err usage: screen [on|off]')).toBeNull();
  });

  it('ctrl reply carries the four color segments', () => {
    const parsed = parseDeviceReply('ok ctrl body=0x1e3b2a button=0xc8a24a accent=0xc8a24a grip=0x16301f');
    expect(parsed.channel).toBe('ctrl');
    expect(parsed.fields).toEqual({ body: 0x1e3b2a, button: 0xc8a24a, accent: 0xc8a24a, grip: 0x16301f });
  });

  it('ctrl reply needs all four segments', () => {
    expect(parseDeviceReply('ctrl body=0x232323')).toBeNull();
    expect(parseDeviceReply('err colors are 0xRRGGBB')).toBeNull();
  });

  it('ds reply carries both switches', () => {
    for (const line of ['ds touchpad=off capture=on', 'ds touchpad=off capture=on (persisted)']) {
      expect(parseDeviceReply(line)).toEqual({
        channel: 'ds',
        fields: { touchpad_plus_minus: false, capture_key: true },
      });
    }
    expect(parseDeviceReply('err usage: ds touchpad|capture [on|off]')).toBeNull();
  });

  it('status reply carries device facts', () => {
    const parsed = parseDeviceReply(STATUS_LINE);
    expect(parsed.channel).toBe('device');
    const fields = parsed.fields;
    expect(fields.firmware).toBe('v0.4.0-12-gabcdef');
    expect(fields.partition).toBe('ota_0');
    expect(fields.ota_state).toBe('idle');
    expect(fields.pairing).toBe('idle');
    expect(fields.role).toBe('device');
    expect(fields.light).toBe(60);
    expect(fields.screen_on).toBe(true);
    expect(fields.uptime_s).toBe(1234);
    expect(fields.heap).toBe(458752);
    expect(fields.heap_total).toBe(524288);
    expect([fields.battery_mv, fields.battery_percent]).toEqual([3971, 85]);
    expect(fields.charging).toBe(false);
    // pad 字段的值里带空格（型号描述）：只保留第一段，但必须仍然存在。
    expect(fields.pad).toBe('DualSense');
  });

  it('status reply from an older firmware keeps free only', () => {
    const parsed = parseDeviceReply('state pairing=idle role=device backlight=60 screen=1 uptime=1234s '
            + 'heap=458752 batt=3971mV/85% chg=0 fw=v0.4.0-12-gabcdef part=ota_0 ota=idle');
    expect(parsed.channel).toBe('device');
    expect(parsed.fields.heap).toBe(458752);
    expect(parsed.fields.heap_total).toBeUndefined();
  });

  it('version reply carries image state', () => {
    const parsed = parseDeviceReply('fw=v0.4.0-12-gabcdef part=ota_1 image=pending-verify ota=idle');
    expect(parsed.channel).toBe('device');
    expect(parsed.fields.partition).toBe('ota_1');
    expect(parsed.fields.image).toBe('pending-verify');
  });

  it('other lines are left alone', () => {
    for (const line of ['', '   ', 'ok key injected', 'ok mem report queued (printed next frame)',
      'I (1234) remapad_cli: cli ready (type help)',
      'pad ui mode on (dpad moves, circle confirms)']) {
      expect(parseDeviceReply(line)).toBeNull();
    }
  });

  it('netlog status line parsed', () => {
    const parsed = parseDeviceReply(
      'netlog state=connected ssid=slime_nest dest=192.168.1.5:9999 frames=10 sent=20 dropped=0',
    );
    expect(parsed.channel).toBe('netlog');
    expect(parsed.fields.state).toBe('connected');
    expect(parsed.fields.ssid).toBe('slime_nest');
    expect(parsed.fields.dest).toBe('192.168.1.5:9999');
  });

  it('netlog off state parsed', () => {
    const parsed = parseDeviceReply('netlog state=off ssid=- dest=- frames=0 sent=0 dropped=0');
    expect(parsed.channel).toBe('netlog');
    expect(parsed.fields.state).toBe('off');
  });

  it('netlog line without state ignored', () => {
    expect(parseDeviceReply('netlog session stopped unexpectedly')).toBeNull();
  });

  it('saved credentials parsed', () => {
    const parsed = parseDeviceReply('ok netlog cred ssid=slime_nest pass=hunter2');
    expect(parsed.channel).toBe('netlog_cred');
    expect(parsed.fields).toEqual({ ssid: 'slime_nest', pass: 'hunter2' });
    expect(parsed.channel).not.toBe('netlog');
  });

  it('unconfigured credentials become empty', () => {
    const parsed = parseDeviceReply('ok netlog cred ssid=- pass=-');
    expect(parsed.channel).toBe('netlog_cred');
    expect(parsed.fields).toEqual({ ssid: '', pass: '' });
  });
});

describe('color input', () => {
  it('accepts with and without prefix', () => {
    expect(parseColor('0x232323')).toBe(0x232323);
    expect(parseColor(' 232323 ')).toBe(0x232323);
    expect(parseColor('0X1E3B2A')).toBe(0x1e3b2a);
  });

  it('rejects junk', () => {
    for (const text of ['', '0x', '1234567', '0xGGGGGG', '黑']) {
      expect(parseColor(text)).toBeNull();
    }
  });

  it('swatch colors pick readable text', () => {
    expect(cssColor(0x1e3b2a)).toBe('#1e3b2a');
    expect(readableOn(0xb9bec4)).toBe('#000000'); // 银灰是亮底
    expect(readableOn(0x232323)).toBe('#ffffff'); // 标准黑是暗底
  });

  it('uptime text keeps two digit fields', () => {
    expect(formatUptime(65)).toBe('01:05');
    expect(formatUptime(3725)).toBe('1:02:05');
  });
});
