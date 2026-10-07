// 反馈帧打印限频与桥接写回限速：都只影响打印与写回节奏，不碰协议。
// 数据与核对状态见 docs/ABSTRACTIONS.md。

import { feedbackParams } from '../link/frame.js';

/** FEEDBACK 载荷 → 一行可读反馈（音频触觉合成与打印共用）。 */
export function formatFeedback(payload) {
  const params = feedbackParams(payload);
  if (params === null) {
    return '反馈帧（载荷过短）';
  }
  let line = `反馈 震动 L=${params.rumbleOn[0] ? 'on' : 'off'} R=${params.rumbleOn[1] ? 'on' : 'off'} `
        + `强度 ${params.lfAmp[0]}/${params.lfAmp[1]} `
        + `玩家灯 0x${params.playerLed.toString(16).padStart(2, '0')} `
        + `触觉 0x${params.sample.toString(16).padStart(2, '0')}`;
    // 高频带（纹理）：解析里已按长度挡掉过短的老固件帧。
  line += ` 高频 ${params.hfAmp[0]}/${params.hfAmp[1]}`;
  if (params.lfFreq !== null) {
    // 两带驱动频率落地值（Hz）：音频触觉合成按它选频。
    line += ` 频率 ${params.lfFreq[0]}/${params.lfFreq[1]}+${params.hfFreq[0]}/${params.hfFreq[1]}`;
  }
  const hd = params.hd;
  if (hd !== null) {
    // HD 时序子帧段：固件按布局行重整出的子帧序列（PC 侧只做哑渲染）。
    line += ` HD ${hd.l.count}+${hd.r.count}子帧${hd.speaker[1] ? ' 发声' : ''}`;
  }
  return line;
}

/** 反馈帧打印限频：震动效果包络里强度逐帧在变，逐条打印会把日志区刷爆
 * （GUI 的日志区尤其扛不住每秒上百条）。窗口内只放行第一条，其余合并计数；
 * 窗口过后的下一条带上「已合并 N 条」。数据面（写回手柄）不受影响，这里只管打印。 */
export class FeedbackThrottle {
  constructor(windowS = 1.0) {
    this.windowS = windowS;
    this.nextOk = 0.0;
    this.suppressed = 0;
  }

  /** 返回要打印的一行；窗口内返回 null。 */
  feed(payload, now) {
    const line = formatFeedback(payload);
    if (now < this.nextOk) {
      this.suppressed += 1;
      return null;
    }
    const merged = this.suppressed;
    this.suppressed = 0;
    this.nextOk = now + this.windowS;
    return merged ? `（已合并 ${merged} 条）${line}` : line;
  }
}

/** 桥接写回限速：游戏内震动包络逐包都变，设备侧「字节变了才发」压不住
 * 写回量，蓝牙 HID 写回又慢，会把会话循环拖到输入转发卡顿。把写回钉在
 * minIntervalS 上限：窗口内只放行第一条，被挡下的帧不丢、留作最新待写帧，
 * 窗口到期由 poll 放行——收尾状态（比如停震的最后一帧）因此一定落地，
 * 马达不会被钉住。 */
export class WriteBackGate {
  constructor(minIntervalS = 0.03) {
    this._min = minIntervalS;
    this._nextOk = 0.0;
    this._pending = null;
  }

  /** 会话循环收到 OUT_REPORT 时调用：true 表示这一帧现在就写。 */
  admit(payload, now) {
    if (!payload.length) {
      return false;
    }
    if (now >= this._nextOk) {
      this._nextOk = now + this._min;
      this._pending = null;
      return true;
    }
    this._pending = payload;
    return false;
  }

  /** 窗口到期后放行最新待写帧；会话循环每轮调用一次。 */
  poll(now) {
    if (this._pending !== null && now >= this._nextOk) {
      const payload = this._pending;
      this._pending = null;
      this._nextOk = now + this._min;
      return payload;
    }
    return null;
  }
}
