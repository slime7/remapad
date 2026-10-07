// 网络调试会话的纯逻辑：UDP 地址解析与链路关闭语义。
import { describe, expect, it } from 'vitest';

import { NETLOG_PORT_DEFAULT, UdpLink, parseEndpoint } from '../src/link/net.js';

describe('parseEndpoint', () => {
  it('ip only uses default port', () => {
    expect(parseEndpoint('192.168.1.5')).toEqual(['192.168.1.5', NETLOG_PORT_DEFAULT]);
  });

  it('ip with port', () => {
    expect(parseEndpoint('192.168.1.5:6000')).toEqual(['192.168.1.5', 6000]);
  });

  it('whitespace tolerated', () => {
    expect(parseEndpoint(' 10.0.0.2 : 9998 ')).toEqual(['10.0.0.2', 9998]);
  });

  it('port out of range rejected', () => {
    expect(() => parseEndpoint('1.2.3.4:70000')).toThrow();
    expect(() => parseEndpoint('1.2.3.4:0')).toThrow();
  });

  it('port not number rejected', () => {
    expect(() => parseEndpoint('1.2.3.4:abc')).toThrow();
  });

  it('empty host rejected', () => {
    expect(() => parseEndpoint(':9999')).toThrow();
    expect(() => parseEndpoint('   ')).toThrow();
  });
});

describe('UdpLink close semantics', () => {
  it('closed link refuses writes', () => {
    const conn = new UdpLink('127.0.0.1', NETLOG_PORT_DEFAULT);
    conn.close();
    expect(() => conn.write(Buffer.from('x'))).toThrow(/UDP 链路已关闭/);
  });

  it('close is idempotent', () => {
    const conn = new UdpLink('127.0.0.1', NETLOG_PORT_DEFAULT);
    conn.close();
    conn.close(); // 关过的链路再关一次不许抛
  });
});
