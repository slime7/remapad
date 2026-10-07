// 蓝牙连接的 DualSense 私有触觉流：有内容时按 10.67ms 节拍把子帧序列渲染成
// 0x32 报告（发声段折进两侧音圈）；给了 speakerEncoder 时发声段改走 0x36 报文。
// 空闲整流停发——常驻空包会和同频段设备互相干扰；写回被拒时经 onError 通知会话。
// 数据与核对状态见 docs/controller-ps.md。

import {
  BT36_SPEAKER_FRAMES,
  BT36_SPEAKER_TAIL_S,
  BT39_INTERVAL_S,
  BT_FRAMES,
  BT_HAPTIC_TAIL_S,
  BT_INTERVAL_S,
  BT_PCM_BYTES,
  bt36BuildReport,
  bt39BuildReport,
  btBuildReport,
  btRenderPcm,
  renderSpeakerBeat,
  renderSpeakerPair,
} from './wire.js';
import { SyncEvent } from '../util.js';
import { VoiceState, hasContent, sideActive } from './synth.js';

/** 蓝牙私有的 DualSense 触觉流发送器。
 *
 * 发送时刻钉在固定网格上（`run`）：渲染与写回的耗时不计入周期，落后超过
 * 一拍就重新对表、不连发追赶；空闲唤醒也从当前时刻重新对表。clock 是取时刻
 * 的入口（默认单调秒），主机端用例用假时钟把整条节拍瞬间跑完。
 */
export class Ds5HapticsBt {
  static LABEL = 'DS5 蓝牙触觉流已启用（0x32 私有报文，HID 震动让位）';
  static LABEL_36 = 'DS5 蓝牙触觉流已启用（0x36 HD 触觉 + 手柄喇叭，HID 震动让位）';

  /**
     * @param device 写回入口（node-hid 设备面：write 返回实交字节数）
     * @param reporter {line,error} 或 null
     * @param options {onError, speakerEncoder, clock, pair}
     */
  constructor(device, reporter = null, {
    onError = null, speakerEncoder = null, clock = () => performance.now() / 1000, pair = false,
  } = {}) {
    this._device = device;
    this._reporter = reporter;
    this._onError = onError;
    this._speakerEncoder = speakerEncoder;
    /** 成对形态（0x39）：一报 2 块触觉 + 2 帧喇叭、节拍 21.33ms。需要编码器。 */
    this._pair = Boolean(pair) && speakerEncoder !== null;
    this._interval = this._pair ? BT39_INTERVAL_S : BT_INTERVAL_S;
    this._clock = clock;
    this._params = {};
    this._state = new VoiceState();
    this._stateBeat = new VoiceState();
    this._lastSpeakerAt = 0.0;
    this._lastCoilAt = 0.0;
    this._stats = {
      writes: 0, writeMsTotal: 0.0, writeMsMax: 0.0, late: 0,
      beatS: BT_INTERVAL_S, short: 0, shortRecovered: 0,
      firstAt: 0.0, lastAt: 0.0,
    };
    /** 起震延迟统计（内容到达 → 首报写出）。 */
    this._pendingSince = null;
    this._onsetMsTotal = 0.0;
    this._onsetMsMax = 0.0;
    this._onsets = 0;
    this._engaged = false;
    this._stop = new SyncEvent();
    this._wake = new SyncEvent();
    this._running = false;
  }

  get active() {
    return this._running;
  }

  get speakerActive() {
    return this._speakerEncoder !== null;
  }

  get label() {
    return this._speakerEncoder !== null ? Ds5HapticsBt.LABEL_36 : Ds5HapticsBt.LABEL;
  }

  /** 写回链路统计（份数、平均/最大单次写回耗时、超节拍份数、实际节拍）。 */
  stats() {
    const s = this._stats;
    if (s.writes === 0) {
      return '写回统计：无写回';
    }
    const avg = s.writeMsTotal / s.writes;
    const spanMs = (s.lastAt - s.firstAt) * 1000.0;
    const beat = s.writes > 1 ? `${(spanMs / (s.writes - 1)).toFixed(2)}ms` : '—';
    const onset = this._onsets
      ? `，起震延迟 平均 ${(this._onsetMsTotal / this._onsets).toFixed(1)}ms`
        + `/最大 ${this._onsetMsMax.toFixed(1)}ms（${this._onsets} 次）`
      : '';
    // 短写：hidapi 用返回值报「实际交给驱动的字节数」，写满才算这一拍真的出去了。
    const short = s.short ? `，短写 ${s.short} 份（重发成功 ${s.shortRecovered}）` : '';
    return `写回统计：${s.writes} 份，平均 ${avg.toFixed(1)}ms/份，最大 ${s.writeMsMax.toFixed(1)}ms，`
            + `超节拍 ${s.late} 份，实际节拍 ${beat}（目标 ${(s.beatS * 1000.0).toFixed(2)}ms）${onset}${short}`;
  }

  /** 启动发送循环（不等待；错误经 onError 上报）。 */
  start() {
    this.run().catch(() => {});
    return true;
  }

  stop() {
    this._stop.set();
    this._wake.set();
    return this._promise;
  }

  setParams(params) {
    this._params = { ...params };
    // 新内容到达即时唤醒空闲的发送线程：短震动的第一拍不等 20ms 兜底轮询。
    if (hasContent(params)) {
      if (this._pendingSince === null) {
        this._pendingSince = this._clock();
      }
      this._wake.set();
    }
  }

  /** 私有流是否已经接到 HD 子帧（真的在驱动音圈）。让位（`haptic audio on`）
     * 要等它置位。 */
  get engaged() {
    return this._engaged;
  }

  _warn(text) {
    if (this._reporter !== null) {
      this._reporter.error(text);
    }
  }

  /** 一条普通提示（仅在 reporter 支持 line 时输出）。 */
  _note(text) {
    if (this._reporter !== null && typeof this._reporter.line === 'function') {
      this._reporter.line(text);
    }
  }

  /** 当前拍音圈是否有内容（任一子帧增益非零）。 */
  static coilActive(leftV, rightV) {
    return leftV !== null && (sideActive(leftV) || sideActive(rightV));
  }

  /** 子帧序列的可读描述（诊断用）：每子帧「低频/增益 + 高频/增益」。 */
  static describe(side) {
    const keys = (side.keys ?? []).slice(0, Math.max(0, side.count));
    if (!keys.length) {
      return '静默';
    }
    return keys.map(([[lf, lg], [hf, hg]]) => `${lf}Hz/${lg}+${hf}Hz/${hg}`).join(' ');
  }

  /** 等到下一拍（到点就是等 0）：返回 true 表示该收尾了（停止位）。 */
  waitUntilDue(due) {
    return this._stop.wait(Math.max(0.0, due - this._clock()));
  }

  /** 空闲等待：内容到达（setParams 唤醒）或兜底轮询超时后返回。 */
  async waitForContent(timeoutS) {
    if (await this._wake.wait(timeoutS)) {
      this._wake.clear();
    }
  }

  /** 发送主循环：到点才产报，周期只由网格决定。写回被拒即退出。 */
  async run() {
    if (this._running) {
      return;
    }
    this._running = true;
    this._promise = undefined;
    const clock = this._clock;
    let nextDue = clock();
    let seq = 0;
    let packetSeq = 0;
    try {
      while (!this._stop.isSet()) {
        // 到点才产报：等待排在渲染之前，写回慢也只让这一拍晚一点发出。
        if (await this.waitUntilDue(nextDue)) {
          return;
        }
        const params = { ...this._params };
        const hd = params.hd != null
          ? [params.hd.l, params.hd.r, [params.hd.speaker]]
          : null;
        if (hd !== null && !this._engaged) {
          // 开关打开与循环启动只说明通路就绪，收到 HD 子帧才开始驱动音圈。
          this._engaged = true;
          const carrier = this._speakerEncoder ? '0x36 HD + 手柄喇叭' : '0x32 音圈';
          this._note(`蓝牙触觉流接到主机的 HD 子帧，开始驱动触觉音圈（${carrier}）：`
                        + `左 ${Ds5HapticsBt.describe(hd[0])} / 右 ${Ds5HapticsBt.describe(hd[1])}`);
        }
        const nowSeconds = clock();
        const leftV = hd !== null ? hd[0] : null;
        const rightV = hd !== null ? hd[1] : null;
        const speaker = hd !== null ? hd[2] : [];
        const tone = speaker[0] ?? [0, 0];
        if (tone[1]) {
          this._lastSpeakerAt = nowSeconds;
        }
        if (Ds5HapticsBt.coilActive(leftV, rightV)) {
          this._lastCoilAt = nowSeconds;
        }
        // 0x36 只在喇叭真有内容（含收音尾）时上；触觉走 0x32；两条静默超尾长
        // 就整流停发。
        const use36 = this._speakerEncoder !== null
                    && nowSeconds - this._lastSpeakerAt < BT36_SPEAKER_TAIL_S;
        const hapticRecent = nowSeconds - this._lastCoilAt < BT_HAPTIC_TAIL_S
                    || nowSeconds - this._lastSpeakerAt < BT_HAPTIC_TAIL_S;
        let report;
        let beatS;
        if (use36) {
          if (this._pair) {
            const coil = leftV === null ? Buffer.alloc(BT_PCM_BYTES * 2)
              : btRenderPcm(leftV, rightV, [], this._state, BT_FRAMES * 2);
            const pairPcm = renderSpeakerPair(tone, this._stateBeat);
            const frameBytes = BT36_SPEAKER_FRAMES * 4;
            const speakerBlock = Buffer.concat([
              this._speakerEncoder.encode(pairPcm.subarray(0, frameBytes)),
              this._speakerEncoder.encode(pairPcm.subarray(frameBytes)),
            ]);
            report = bt39BuildReport(coil, speakerBlock, seq, packetSeq);
          } else {
            const coil = leftV === null ? Buffer.alloc(BT_PCM_BYTES)
              : btRenderPcm(leftV, rightV, [], this._state);
            const speakerBlock = this._speakerEncoder.encode(renderSpeakerBeat(tone, this._stateBeat));
            report = bt36BuildReport(coil, speakerBlock, seq, packetSeq);
          }
          seq = (seq + 1) & 0xf;
          packetSeq = (packetSeq + 1) & 0xff;
          beatS = this._pair ? this._interval : BT_INTERVAL_S;
        } else if (hapticRecent) {
          const pcm = leftV === null ? Buffer.alloc(BT_PCM_BYTES)
            : btRenderPcm(leftV, rightV, speaker, this._state);
          report = btBuildReport(pcm, seq);
          seq = (seq + 1) & 0xff;
          beatS = BT_INTERVAL_S;
        } else {
          // 空闲：一报不发，等 setParams 的内容唤醒或 20ms 兜底轮询；醒来把
          // 节拍网格挪到当前时刻：空闲时长不定，续用空闲前的网格会连着补几拍。
          await this.waitForContent(0.02);
          nextDue = clock();
          continue;
        }
        try {
          const t0 = clock();
          const written = await this._device.write(report);
          const done = clock();
          const writeMs = (done - t0) * 1000.0;
          const s = this._stats;
          // Windows 的 hidapi 会把短于描述符声明长度的写回补齐到
          // OutputReportByteLength（DS5 蓝牙集合声明 547）——返回值比报告长
          // 是常态，只有真的少交（< 报告长度）才是这一拍没进队列。
          if (typeof written === 'number' && written < report.length) {
            s.short += 1;
            if (s.short === 1) {
              this._note(`蓝牙触觉流首份短写：${written}/${report.length} 字节`
                                + '——输出队列没收下这一拍，手柄这段收不到');
            }
            // 短写是「驱动没把这一份收进队列」：立刻原样重发一次，能把
            // 队列瞬时满丢掉的拍救回来（内容仍是这一段波形，相位不跳）。
            let retry = -1;
            try {
              retry = await this._device.write(report);
            } catch {
              retry = -1;
            }
            if (typeof retry === 'number' && retry >= report.length) {
              s.shortRecovered += 1;
            }
          }
          if (s.writes === 0) {
            s.firstAt = t0;
          }
          s.writes += 1;
          s.lastAt = done;
          s.writeMsTotal += writeMs;
          s.writeMsMax = Math.max(s.writeMsMax, writeMs);
          s.beatS = beatS;
          if (writeMs > beatS * 1000.0) {
            s.late += 1;
          }
          // 起震延迟：内容到达（setParams 置位）到首报写出之间的时间。
          if (this._pendingSince !== null) {
            const onsetMs = Math.max(0.0, (done - this._pendingSince) * 1000.0);
            this._onsetMsTotal += onsetMs;
            this._onsetMsMax = Math.max(this._onsetMsMax, onsetMs);
            this._onsets += 1;
            this._pendingSince = null;
          }
        } catch (exc) {
          this._warn(`DS5 蓝牙触觉流写回失败：${exc.message}`);
          if (this._onError !== null) {
            this._onError(exc);
          }
          return;
        }
        nextDue += beatS;
        const after = clock();
        if (after - nextDue > beatS) {
          // 落后超过一拍（系统挂起、写回卡住）：网格挪到当前时刻，不做连发追赶。
          nextDue = after;
        }
      }
    } finally {
      this._running = false;
    }
  }
}
