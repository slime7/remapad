// amiibo 上传状态机：BEGIN → 一段 DATA → END，逐帧 ACK 驱动；由会话
// 主循环 tick 驱动，桥接转发同时照跑。镜像固定 540 字节（三个数据帧），
// 设备逐帧回 ACK，received 就是续传起点。
// 数据与核对状态见 docs/ABSTRACTIONS.md。

import {
  AMIIBO_DATA_MAX,
  AMIIBO_STATE_NAMES,
  AMIIBO_CODE_NAMES,
  TYPE_AMIIBO_BEGIN,
  TYPE_AMIIBO_DATA,
  TYPE_AMIIBO_END,
  WIRE_MAX_PAYLOAD,
  amiiboBeginPayload,
  amiiboDataPayload,
  encode,
} from '../link/frame.js';
import { now } from '../util.js';

export const AMIIBO_STATE_RECEIVING = 1;
export const AMIIBO_STATE_DONE = 2;
export const AMIIBO_STATE_FAILED = 3;
/** ACK 里 slot 字段的「未落库」取值。 */
export const AMIIBO_SLOT_NONE = 0xff;

export function describeAmiiboAck(ack) {
  return `state=${ack.state} code=${ack.code} received=${ack.received}`;
}

export class AmiiboJob {
  static ACK_TIMEOUT_S = 3.0;
  static MAX_RETRIES = 5;

  /** @param {string} name @param {Buffer} data */
  constructor(name, data, send, reporter) {
    this.name = name;
    this.data = data;
    this._send = send;
    this.reporter = reporter;
    this.received = 0;
    this.retries = 0;
    this.phase = 'begin';
    this.deadline = 0.0;
    this.finished = false;
    this.exitCode = 1;
  }

  start(nowSeconds) {
    this.reporter.line(`上传 amiibo「${this.name}」（${this.data.length} 字节）`);
    this.reporter.event('amiibo_started', { amiibo: this.name, size: this.data.length });
    this._send(encode(TYPE_AMIIBO_BEGIN, 0, 0, amiiboBeginPayload(this.name, this.data.length)));
    this.phase = 'begin';
    this.deadline = nowSeconds + AmiiboJob.ACK_TIMEOUT_S;
  }

  /** 从设备已确认的字节起重发剩余数据；设备对已收区间按幂等处理。 */
  _sendPending(nowSeconds) {
    const window = [];
    let offset = this.received;
    while (offset < this.data.length) {
      const piece = this.data.subarray(offset, offset + AMIIBO_DATA_MAX);
      window.push(encode(TYPE_AMIIBO_DATA, 0, 0, amiiboDataPayload(offset, piece), WIRE_MAX_PAYLOAD));
      offset += piece.length;
    }
    this._send(Buffer.concat(window));
    this.deadline = nowSeconds + AmiiboJob.ACK_TIMEOUT_S;
  }

  tick(nowSeconds) {
    if (this.finished || nowSeconds < this.deadline) {
      return;
    }
    if (this.phase === 'begin') {
      this._fail('设备没有回应上传请求；确认设备是 COM 模式、串口没被占用'
                + '且固件已带 amiibo 功能');
      return;
    }
    if (this.phase === 'data') {
      this.retries += 1;
      if (this.retries > AmiiboJob.MAX_RETRIES) {
        this._fail(`连续 ${this.retries} 次没有等到数据应答，上传中止；`
                    + '设备侧 5 秒无数据也会自行作废会话');
        return;
      }
      this.reporter.line(`数据应答超时，从 ${this.received} 字节处重发（第 ${this.retries} 次）`);
      this._sendPending(nowSeconds);
      return;
    }
    if (this.phase === 'end') {
      this._fail('设备没有确认收尾，槽位没有落库');
    }
  }

  onAck(ack) {
    if (this.finished) {
      return;
    }
    if (this.phase === 'begin') {
      if (ack.stateId !== AMIIBO_STATE_RECEIVING || ack.codeId !== 0) {
        this._fail(`设备拒绝上传（${describeAmiiboAck(ack)}）`);
        return;
      }
      this.phase = 'data';
      this._sendPending(now());
      return;
    }
    if (this.phase === 'data') {
      if (ack.stateId === AMIIBO_STATE_FAILED) {
        this._fail(`设备中止上传（${describeAmiiboAck(ack)}），已确认 ${ack.received} 字节`);
        return;
      }
      this.retries = 0;
      this.received = ack.received;
      if (this.received >= this.data.length) {
        this.phase = 'end';
        this._send(encode(TYPE_AMIIBO_END, 0, 0));
        this.deadline = now() + AmiiboJob.ACK_TIMEOUT_S;
      }
      return;
    }
    if (this.phase === 'end') {
      if (ack.stateId === AMIIBO_STATE_DONE && ack.codeId === 0) {
        this.finished = true;
        this.exitCode = 0;
        const slot = ack.slot;
        this.reporter.line(`上传完成：「${this.name}」已存为槽位 `
                    + `${slot === AMIIBO_SLOT_NONE ? '-' : slot}`);
        this.reporter.event('amiibo_finished', { ok: true, slot, amiibo: this.name });
      } else {
        this._fail(`设备落库失败（${describeAmiiboAck(ack)}）`);
      }
    }
  }

  _fail(message) {
    this.reporter.error(message);
    this.reporter.event('amiibo_finished', { ok: false, message });
    this.finished = true;
    this.exitCode = 1;
  }
}

/** 供测试与调用方按同一构造补齐 ACK 字段。 */
export function makeAmiiboAck(state, code, received, slot = 0xff) {
  return {
    stateId: state,
    codeId: code,
    received,
    slot,
    state: AMIIBO_STATE_NAMES[state] ?? String(state),
    code: AMIIBO_CODE_NAMES[code] ?? String(code),
  };
}
