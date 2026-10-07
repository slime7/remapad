// OTA 升级窗口状态机：由会话主循环驱动，支持串口与网络 UDP 通道。
// 数据与核对状态见 docs/ABSTRACTIONS.md。

import {
  OTA_SLOT_WINDOW_END,
  OTA_STATE_NAMES,
  OTA_CODE_NAMES,
  TYPE_OTA_BEGIN,
  TYPE_OTA_DATA,
  TYPE_OTA_END,
  WIRE_MAX_PAYLOAD,
  encode,
  otaBeginPayload,
  otaDataPayload,
} from '../link/frame.js';
import { now } from '../util.js';

export const OTA_STATE_RECEIVING = 1;
export const OTA_STATE_DONE = 2;
export const OTA_STATE_FAILED = 3;
/** 设备侧 5 秒无数据作废会话时回的错误码（固件 ota_proto.h 的 ota_code_t）。 */
export const OTA_CODE_TIMEOUT = 7;

export function describeAck(ack) {
  const version = ack.version ? `，设备在跑 ${ack.version}` : '';
  return `state=${ack.state} code=${ack.code}${version}`;
}

/** OTA 升级窗口状态机。 */
export class OtaJob {
  /** BEGIN 总窗：设备预擦目标分区可达数秒。 */
  static BEGIN_ACK_TIMEOUT_S = 20.0;
  /** BEGIN 重发间隔（设备端同尺寸幂等）。 */
  static BEGIN_RETRY_S = 2.0;
  /** 窗口应答等待：重发要赶在设备 5 秒空闲作废窗内到达。 */
  static ACK_TIMEOUT_S = 0.3;
  /** END 总窗：设备校验镜像 + 重启。 */
  static END_ACK_TIMEOUT_S = 30.0;
  /** END 重发间隔：设备 5 秒空闲作废窗内必须再到达一次。 */
  static END_RETRY_S = 2.5;
  static MAX_WINDOW_RETRIES = 12;
  /** 设备侧超时作废后从头重来的次数上限。 */
  static MAX_SESSION_RESTARTS = 3;

  /**
     * @param {Buffer} image
     * @param {string} version
     * @param {(data: Buffer) => void} send
     * @param {{line: Function, error: Function, event: Function}} reporter
     */
  constructor(image, version, send, reporter, lossy = false) {
    this.image = image;
    this.version = version;
    this._send = send;
    this.reporter = reporter;
    /** lossy（UDP）：收尾应答彻底没等到时按「不确定完成」收场（exit 0），
         * 交给 --wait / 重连后的 version 确认；串口维持硬失败。 */
    this.lossy = lossy;
    this.confirmed = 0;
    this.nextSeq = 0;
    this.retries = 0;
    this.restarts = 0;
    this.phase = 'begin';
    this.deadline = 0.0;
    this.retryDeadline = 0.0;
    this.printedPct = -1;
    this.finished = false;
    this.exitCode = 1;
  }

  start(nowSeconds) {
    this.reporter.line(`写入 ${this.image.length} 字节（镜像版本 ${this.version}）`);
    this.deadline = nowSeconds + OtaJob.BEGIN_ACK_TIMEOUT_S;
    this._sendBegin(nowSeconds);
  }

  _sendBegin(nowSeconds) {
    this._send(encode(TYPE_OTA_BEGIN, 0, 0, otaBeginPayload(this.image.length), WIRE_MAX_PAYLOAD));
    this.phase = 'begin';
    this.retryDeadline = nowSeconds + OtaJob.BEGIN_RETRY_S;
  }

  _sendWindow(nowSeconds) {
    let offset = this.confirmed;
    let seq = this.nextSeq;
    const frames = [];
    while (frames.length < 16 && offset < this.image.length) {
      const piece = this.image.subarray(offset, offset + 200);
      frames.push([seq & 0xffff, piece]);
      offset += piece.length;
      seq += 1;
    }
    const window = [];
    for (const [index, [frameSeq, piece]] of frames.entries()) {
      const slot = index === frames.length - 1 ? OTA_SLOT_WINDOW_END : 0;
      window.push(encode(TYPE_OTA_DATA, slot, frameSeq & 0xff, otaDataPayload(frameSeq, piece), WIRE_MAX_PAYLOAD));
    }
    this._send(Buffer.concat(window));
    this.deadline = nowSeconds + OtaJob.ACK_TIMEOUT_S;
  }

  tick(nowSeconds) {
    if (this.finished) {
      return;
    }
    if (this.phase === 'begin') {
      // deadline 是放弃时刻（总窗），retryDeadline 才是重发节拍。
      if (nowSeconds >= this.deadline) {
        this._fail(`设备没有在 ${OtaJob.BEGIN_ACK_TIMEOUT_S.toFixed(0)} 秒内回应 BEGIN；`
                    + '确认设备可达（串口没被占用 / WiFi 会话开着且目标已学习）');
        return;
      }
      if (nowSeconds >= this.retryDeadline) {
        this._sendBegin(nowSeconds);
      }
      return;
    }
    if (this.phase === 'data') {
      // 这里 deadline 是本窗应答的等待时刻（到了就整窗重发）。
      if (nowSeconds < this.deadline) {
        return;
      }
      this.retries += 1;
      if (this.retries > OtaJob.MAX_WINDOW_RETRIES) {
        this._fail(`连续 ${this.retries} 个窗口没有应答，升级中止；`
                    + '设备侧 5 秒无数据会自行作废会话，仍从旧镜像启动');
        return;
      }
      this.reporter.line(`窗口应答超时，从 ${this.confirmed} 字节处重发（第 ${this.retries} 次）`);
      this._sendWindow(nowSeconds);
      return;
    }
    if (this.phase === 'end') {
      if (nowSeconds >= this.deadline) {
        this._finishUncertain();
        return;
      }
      // done 应答丢失而设备已在重启时，重发的 END 不会有回应，静默补发即可。
      if (nowSeconds >= this.retryDeadline) {
        this._send(encode(TYPE_OTA_END, 0, 0));
        this.retryDeadline = nowSeconds + OtaJob.END_RETRY_S;
      }
    }
  }

  onAck(ack) {
    if (this.finished) {
      return;
    }
    if (this.phase === 'begin') {
      if (ack.stateId !== OTA_STATE_RECEIVING || ack.codeId !== 0) {
        this._fail(`设备拒绝升级（${describeAck(ack)}）；设备忙或镜像被拒时稍后重试`);
        return;
      }
      if (ack.version) {
        this.reporter.line(`设备当前版本 ${ack.version} → 写入 ${this.version}`);
      }
      this.phase = 'data';
      this._sendWindow(now());
      return;
    }
    if (this.phase === 'data') {
      if (ack.stateId === OTA_STATE_FAILED) {
        this._restartOrFail(ack);
        return;
      }
      if (ack.received <= this.confirmed) {
        // 无进展的应答（设备写 flash 停顿触发的整窗重发、其序号错误应答，
        // 或迟到的旧应答）：只前进不回卷、也不重复驱动窗口。
        return;
      }
      this.retries = 0;
      this.confirmed = ack.received;
      this.nextSeq = ack.nextSeq;
      const pct = Math.floor((this.confirmed * 100) / Math.max(this.image.length, 1));
      if (pct !== this.printedPct) {
        this.printedPct = pct;
        this.reporter.line(`  写入 ${String(pct).padStart(3)}%（${this.confirmed}/${this.image.length} 字节）`);
        this.reporter.event('ota_progress', { confirmed: this.confirmed, total: this.image.length });
      }
      const nowSeconds = now();
      if (this.confirmed >= this.image.length) {
        this.phase = 'end';
        this._send(encode(TYPE_OTA_END, 0, 0));
        this.deadline = nowSeconds + OtaJob.END_ACK_TIMEOUT_S;
        this.retryDeadline = nowSeconds + OtaJob.END_RETRY_S;
        this.reporter.line('数据传输完成，等待设备校验镜像');
        return;
      }
      this._sendWindow(nowSeconds);
      return;
    }
    if (this.phase === 'end') {
      if (ack.stateId === OTA_STATE_DONE && ack.codeId === 0) {
        this.finished = true;
        this.exitCode = 0;
        this.reporter.line('升级完成：设备切到新分区并重启，首次启动会先处于「待验证」状态');
        this.reporter.event('ota_finished', { ok: true, message: '' });
        return;
      }
      if (ack.stateId !== OTA_STATE_FAILED) {
        // 设备校验镜像期间迟到的数据面应答（整窗重发的序号错误应答等）：
        // 真正的收尾失败只会以 FAILED 报上来，其余等 DONE 或超时。
        return;
      }
      this._fail(`升级失败（${describeAck(ack)}）；设备仍从旧镜像启动`);
    }
  }

  /** data 阶段收到 FAILED：只有设备侧空闲超时作废可自动从头重来（镜像没写坏，
     * 重走 BEGIN 会开新会话），其余错误码终止。 */
  _restartOrFail(ack) {
    if (ack.codeId !== OTA_CODE_TIMEOUT || this.restarts >= OtaJob.MAX_SESSION_RESTARTS) {
      this._fail(`设备中止升级（${describeAck(ack)}），已收到 ${ack.received} 字节`);
      return;
    }
    this.restarts += 1;
    this.reporter.line(`设备侧接收超时作废，从头重来（第 ${this.restarts} 次）`);
    this.confirmed = 0;
    this.nextSeq = 0;
    this.retries = 0;
    this.printedPct = -1;
    const nowSeconds = now();
    this.deadline = nowSeconds + OtaJob.BEGIN_ACK_TIMEOUT_S;
    this._sendBegin(nowSeconds);
  }

  /** END 总窗内没有任何应答：串口上按失败收场；UDP 上多半是 done 应答丢失、
     * 设备已在重启（后续重发的 END 无人接收），按不确定完成收场，交给
     * --wait 或重连后的 version 确认实际版本。 */
  _finishUncertain() {
    this.finished = true;
    if (!this.lossy) {
      this.reporter.error(`设备没有在 ${OtaJob.END_ACK_TIMEOUT_S.toFixed(0)} 秒内确认收尾`);
      this.reporter.event('ota_finished', { ok: false, message: 'end ack timeout' });
      this.exitCode = 1;
      return;
    }
    this.exitCode = 0;
    this.reporter.line('总窗内没等到收尾应答（应答丢失或设备已在重启）；'
            + '请用 --wait 或重新连接后敲 version 确认在跑的版本');
    this.reporter.event('ota_finished', { ok: true, message: 'end ack lost; verify version after reboot' });
  }

  _fail(message) {
    this.reporter.error(message);
    this.reporter.event('ota_finished', { ok: false, message });
    this.finished = true;
    this.exitCode = 1;
  }
}

/** 供测试与调用方按同一构造补齐 ACK 字段。 */
export function makeOtaAck(state, code, nextSeq, received, version = '') {
  return {
    stateId: state,
    codeId: code,
    nextSeq,
    received,
    state: OTA_STATE_NAMES[state] ?? String(state),
    code: OTA_CODE_NAMES[code] ?? String(code),
    version,
  };
}
