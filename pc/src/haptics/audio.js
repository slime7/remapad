// USB 直插 DualSense 的 4ch 音频触觉流：对扬声器端点开 RtAudio 共享流，
// 频道 3/4 是触觉音圈、1/2 是小喇叭；子帧驱动的哑渲染跑在节拍循环里。
// 数据与核对状态见 docs/controller-ps.md。

import { createRequire } from 'node:module';

import {
  AMP_PEAK_USB,
  CHANNELS,
  RATE,
  VoiceState,
  clamp16,
  hdVoices,
  legacyVoices,
  renderKeys,
  renderSide,
  renderSpeaker,
  sliceSamples,
} from './synth.js';

const require = createRequire(import.meta.url);

/** 每块样本数（10ms）：与 audify 开流的 frameSize 一致。 */
const BLOCK_FRAMES = 480;
const BLOCK_S = BLOCK_FRAMES / RATE;

/** 一条对着 DualSense 音频端点的 4ch int16 输出流 + 子帧驱动的哑渲染。
 *
 * setParams 可从会话循环任意调用，合成按块推进、换参数不重置（拼接处不跳变）。
 * RtAudio 是推模式：节拍循环按 BLOCK_S 网格渲染整块并写入队列，渲染与写队列
 * 的耗时不计入周期。 */
export class Ds5HapticsAudio {
  static LABEL = 'DS5 音频触觉已启用（频道 3/4 触觉、1/2 发声，HID 震动让位）';

  constructor(reporter = null) {
    this._reporter = reporter;
    this._params = {};
    this._state = new VoiceState();
    this._rt = null;
    this._timer = null;
    this._nextDue = 0.0;
    this._runs = 0;
  }

  get active() {
    return this._rt !== null;
  }

  /** 找到 DualSense 端点并开流；任何一步不成立都返回 Promise<false>（回落 HID）。 */
  async start() {
    let mod;
    try {
      mod = require('audify');
    } catch (exc) {
      this._warn(`音频触觉依赖不可用（${exc.message}），回落 HID 震动写回`);
      return false;
    }
    const device = findDs5Device(mod);
    if (device === null) {
      this._warn('没找到 DualSense 音频端点，回落 HID 震动写回');
      return false;
    }
    try {
      this._rt = new mod.RtAudio(mod.RtAudioApi.WINDOWS_WASAPI);
      this._rt.openStream(
        { deviceId: device.id, nChannels: CHANNELS, firstChannel: 0 },
        null,
        mod.RtAudioFormat.RTAUDIO_SINT16,
        RATE,
        BLOCK_FRAMES,
        'remapad-ds5-haptics',
        null,
        null,
      );
      this._rt.start();
    } catch (exc) {
      this._warn(`DualSense 音频端点打开失败（${exc.message}），回落 HID 震动写回`);
      this._rt = null;
      return false;
    }
    // 节拍网格：渲染与写队列落在块与块之间，落后超过一块就重新对表、不连发追赶。
    this._nextDue = performance.now() / 1000 + BLOCK_S;
    const tick = () => {
      if (this._rt === null) {
        return;
      }
      const at = performance.now() / 1000;
      if (at >= this._nextDue - BLOCK_S / 4) {
        this._nextDue += BLOCK_S;
        if (at - this._nextDue > BLOCK_S) {
          this._nextDue = at + BLOCK_S;
        }
        try {
          this._rt.write(this.renderBlock(BLOCK_FRAMES));
        } catch {
          // 渲染异常不拖垮流：下一块照常。
        }
      }
      this._timer = setTimeout(tick, 2);
    };
    this._timer = setTimeout(tick, BLOCK_S * 1000);
    return true;
  }

  stop() {
    if (this._timer !== null) {
      clearTimeout(this._timer);
      this._timer = null;
    }
    if (this._rt !== null) {
      try {
        this._rt.stop();
        this._rt.closeStream();
      } catch {
        // 收尾路径不抛
      }
      this._rt = null;
    }
  }

  /** 吃 link.feedbackParams 的解析结果：HD 时序子帧（固件已按布局重整）
     * 或老固件的两带振幅与频率落地值。 */
  setParams(params) {
    this._params = { ...params };
  }

  /** 音频流是否已经接到 HD 子帧（固件按布局行重整过时序子帧）。让位
     * （`haptic audio on`）要等它置位。 */
  get engaged() {
    return this._params.hd != null;
  }

  /** 渲染一整块 4ch 交错 int16（频道 1/2 扬声器、3/4 左右音圈）。 */
  renderBlock(frames) {
    const hd = hdVoices(this._params);
    const peak = AMP_PEAK_USB;
    let left;
    let right;
    let speaker;
    if (hd !== null) {
      const [leftV, rightV, sp] = hd;
      speaker = sp;
      const sliceSize = sliceSamples(RATE);
      left = renderKeys(leftV, this._state.keyPhase[0], this._state.cursor[0], frames, RATE, peak, sliceSize,
        this._state.gates[0]);
      right = renderKeys(rightV, this._state.keyPhase[1], this._state.cursor[1], frames, RATE, peak, sliceSize,
        this._state.gates[1]);
    } else {
      const [leftV, rightV, sp] = legacyVoices(this._params);
      speaker = sp;
      left = renderSide(leftV, this._state.keyPhase[0], frames, RATE, peak);
      right = renderSide(rightV, this._state.keyPhase[1], frames, RATE, peak);
    }
    const sp = renderSpeaker(speaker[0] ?? [0, 0], this._state, frames, RATE, peak);
    // 发声段折进触觉两路：蓝牙上没有扬声器通道，音圈是发声段唯一的载体，
    // 两条承载通路对音圈的驱动保持一致；USB 的频道 1/2 照常加一份真声。
    for (let i = 0; i < frames; i++) {
      left[i] = clamp16(left[i] + sp[i]);
      right[i] = clamp16(right[i] + sp[i]);
    }
    const block = Buffer.alloc(frames * CHANNELS * 2);
    for (let i = 0; i < frames; i++) {
      block.writeInt16LE(sp[i], i * 8);
      block.writeInt16LE(sp[i], i * 8 + 2);
      block.writeInt16LE(left[i], i * 8 + 4);
      block.writeInt16LE(right[i], i * 8 + 6);
    }
    return block;
  }

  /** 测试与回调共用的整块落位：往调用方给的 Buffer 里写 4ch 交错 int16。 */
  callback(out, frames) {
    this.renderBlock(frames).copy(out);
  }

  _warn(text) {
    if (this._reporter !== null) {
      this._reporter.error(text);
    }
  }
}

/** 在所有 hostapi 里找 DualSense 的输出端点：要求至少 4 个输出通道，
 * 优先 WASAPI 实例（延迟低、通道映射直）。 */
function findDs5Device(mod) {
  const instances = [];
  try {
    instances.push(new mod.RtAudio(mod.RtAudioApi.WINDOWS_WASAPI));
  } catch {
    // WASAPI 起不来就按默认 API 找。
  }
  instances.push(new mod.RtAudio());
  for (const rt of instances) {
    let fallback = null;
    for (const dev of rt.getDevices()) {
      if (!dev.name.toLowerCase().includes('dualsense') || dev.outputChannels < CHANNELS) {
        continue;
      }
      if (rt.getApi().toLowerCase().includes('wasapi')) {
        return dev;
      }
      if (fallback === null) {
        fallback = dev;
      }
    }
    if (fallback !== null) {
      return fallback;
    }
  }
  return null;
}
