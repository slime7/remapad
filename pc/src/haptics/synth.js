// DS5 音频触觉的 PC 侧合成：与固件 haptic_synth.c 同刻度的哑渲染。
// 声部参数来自设备 FEEDBACK 帧的 57 字节 HD 版（固件已按布局行重整好，这里只做
// 哑渲染；老固件的 16 字节帧回落两带正弦）。
// 报文布局、包络门与让位语义见 docs/controller-ps.md。

/** 与固件 haptic_synth.c 同刻度：gain 255 的 int16 峰值（USB 承载与 0x36 的
 * 48kHz 喇叭块共用，与 DS5 布局行的 amp_peak 同值）。 */
export const AMP_PEAK_USB = 30000;
/** 蓝牙私有流承载 8-bit PCM，峰值按 s8 上限留 1。 */
export const AMP_PEAK_BT = 127;
export const RATE = 48000;
export const CHANNELS = 4;
/** 频率缺省值（设备发的落地值理论上不为 0，这里兜底；与固件布局行的
 * lf/hf_default_hz 同源：BlueRetro 驱动常量 0x180/0x1E1 的落地值 80/135Hz）。 */
export const FREQ_DEFAULTS = [80.0, 135.0];
/** 每侧时序子帧上限（与固件 PAD_HD_KEY_MAX 一致：NS2 波形规则为 3）。 */
export const KEY_MAX = 3;
/** 子帧序列的整周期（ms）：3 个子帧各播 1/3，与固件 DS5 布局行的 cycle_ms 同值。 */
export const CYCLE_MS = 15.0;
/** 发声段音色的起音/收音时长（秒）：段边界硬切满幅/零幅会在小喇叭与音圈上
 * 听成咔哒，包络在边沿内平滑过渡。 */
export const SPEAKER_ATTACK_S = 0.006;
export const SPEAKER_RELEASE_S = 0.014;
/** 音圈包络门的起音/收音时长（秒）：起音 1ms 爬满、收音 15ms 锁定最后发声的
 * 子帧淡出，避免块对齐硬切把短震动截没；与固件 haptic_synth 同一条曲线。 */
export const COIL_ATTACK_S = 0.001;
export const COIL_RELEASE_S = 0.015;
/** 发声段音色的二次谐波比例与合成峰值回缩：给蜂鸣一点中空腔体，接近
 * Joy-Con 提示音的音色；谐波频率超过承载奈奎斯特频率时自动省去。 */
export const SPEAKER_HARMONIC2 = 0.22;
/** 基频+谐波的最坏相位叠加，压回峰值刻度。 */
const SPEAKER_SHAPE = 1.0 / 1.09;
const TAU = 2.0 * Math.PI;

export function clamp16(value) {
  return Math.max(-32768, Math.min(32767, value));
}

/** 每个子帧的样本数 = rate × cycle_ms / 1000 / 3（48kHz 下 240、3kHz 下 15）。 */
export function sliceSamples(rate) {
  return Math.max(1, Math.round((rate * CYCLE_MS) / 1000.0 / 3.0));
}

/** HD 段 → [左子帧序列, 右子帧序列, 扬声器]；没有 HD 段返回 null。 */
export function hdVoices(params) {
  const hd = params.hd;
  if (hd == null) {
    return null;
  }
  return [hd.l, hd.r, [hd.speaker]];
}

/** 老固件 16 字节帧回落两带正弦（扬声器恒零）：每侧两条同时叠加的正弦。 */
export function legacyVoices(params) {
  const lfFreq = params.lfFreq ?? FREQ_DEFAULTS;
  const hfFreq = params.hfFreq ?? FREQ_DEFAULTS;
  const lfAmp = params.lfAmp ?? [0, 0];
  const hfAmp = params.hfAmp ?? [0, 0];
  const left = [[lfFreq[0] || FREQ_DEFAULTS[0], lfAmp[0]], [hfFreq[0] || FREQ_DEFAULTS[0], hfAmp[0]]];
  const right = [[lfFreq[1] || FREQ_DEFAULTS[0], lfAmp[1]], [hfFreq[1] || FREQ_DEFAULTS[1], hfAmp[1]]];
  return [left, right, []];
}

/** 一组同时发声的正弦叠加：相位逐样本推进（换参数不重置，拼接处不跳变）。 */
export function renderSide(tones, phases, frames, rate, peak) {
  const gains = tones.map(([_freq, gain]) => (gain * peak) / 255.0);
  const steps = tones.map(([freq]) => (TAU * freq) / rate);
  const out = new Array(frames).fill(0);
  for (let i = 0; i < frames; i++) {
    let total = 0.0;
    for (let k = 0; k < tones.length; k++) {
      if (gains[k] <= 0.0) {
        continue;
      }
      total += gains[k] * Math.sin(phases[k]);
      phases[k] = (phases[k] + steps[k]) % TAU;
    }
    out[i] = clamp16(Math.round(total));
  }
  return out;
}

/** 一条时序子帧序列的渲染：首帧从子帧 0 起播整一切片，其后每 sliceSamples 帧
 * 切下一子帧（回绕），回绕长度就是该侧声明的子帧数——主机是 200Hz 的单子帧流
 * （实抓 94% 的包只声明 1 个子帧），声明之外的槽位不占时间；固定按 3 槽轮播会
 * 把持续震动切成「5ms 有声 + 10ms 静默」的 66Hz 断续。相位跨块与跨子帧都连续
 * （切子帧只换频率与增益，不重置相位）。
 *
 * gate 是音圈包络门（`CoilGate`，跨块连续），不传按直渲处理（主机收震的下一块
 * 立刻全静）：带增益的参数到达且门开着（env 已落到 0）时游标与相位回零——新震动
 * 从自己的第一个子帧出去；主机收震后锁定最后发声的子帧按 COIL_RELEASE_S 淡出。
 * 门控只看参数级的有/无增益，子帧序列内部的静默切片不参与（时间轴不变）。
 */
export function renderKeys(side, phases, cursor, frames, rate, peak, sliceSize, gate = null) {
  const count = side.count;
  const slots = count >= 1 && count <= KEY_MAX ? count : KEY_MAX;
  const keys = side.keys;
  let [lfPhase, hfPhase] = phases;
  let [idx, left] = cursor;
  if (left === 0 || idx >= slots) {
    idx = 0;
    left = sliceSize;
  }
  const out = new Array(frames).fill(0);
  if (gate === null) {
    for (let i = 0; i < frames; i++) {
      if (left === 0) {
        idx = (idx + 1) % slots;
        left = sliceSize;
      }
      left -= 1;
      let total = 0.0;
      if (idx < keys.length) {
        const [[lf, lg], [hf, hg]] = keys[idx];
        if (lg) {
          total += ((lg * peak) / 255.0) * Math.sin(lfPhase);
          lfPhase = (lfPhase + (TAU * lf) / rate) % TAU;
        }
        if (hg) {
          total += ((hg * peak) / 255.0) * Math.sin(hfPhase);
          hfPhase = (hfPhase + (TAU * hf) / rate) % TAU;
        }
      }
      out[i] = clamp16(Math.round(total));
    }
    cursor[0] = idx;
    cursor[1] = left;
    phases[0] = lfPhase;
    phases[1] = hfPhase;
    return out;
  }
  let activeKey = null;
  for (let k = 0; k < Math.min(keys.length, slots); k++) {
    const [[, lg], [, hg]] = keys[k];
    if (lg || hg) {
      activeKey = keys[k];
      break;
    }
  }
  if (activeKey !== null) {
    gate.latch = activeKey;
  } else if (gate.env <= 0.0) {
    gate.latch = null;
  }
  const target = activeKey !== null ? 1.0 : 0.0;
  if (target === 1.0 && gate.env <= 0.0) {
    idx = 0;
    left = sliceSize;
    lfPhase = 0.0;
    hfPhase = 0.0;
  }
  const rise = 1.0 / Math.max(1.0, COIL_ATTACK_S * rate);
  const fall = 1.0 / Math.max(1.0, COIL_RELEASE_S * rate);
  let env = gate.env;
  for (let i = 0; i < frames; i++) {
    if (left === 0) {
      idx = (idx + 1) % slots;
      left = sliceSize;
    }
    left -= 1;
    if (target > env) {
      env = Math.min(1.0, env + rise);
    } else if (target < env) {
      env = Math.max(0.0, env - fall);
    }
    let total = 0.0;
    if (env > 0.0) {
      const key = target === 1.0 ? (idx < keys.length ? keys[idx] : null) : gate.latch;
      if (key !== null) {
        const [[lf, lg], [hf, hg]] = key;
        if (lg) {
          total += ((lg * peak) / 255.0) * Math.sin(lfPhase);
          lfPhase = (lfPhase + (TAU * lf) / rate) % TAU;
        }
        if (hg) {
          total += ((hg * peak) / 255.0) * Math.sin(hfPhase);
          hfPhase = (hfPhase + (TAU * hf) / rate) % TAU;
        }
      }
    }
    out[i] = clamp16(Math.round(total * env));
  }
  gate.env = env;
  cursor[0] = idx;
  cursor[1] = left;
  phases[0] = lfPhase;
  phases[1] = hfPhase;
  return out;
}

/** 单侧音圈的包络门（跨块连续）：env 是 0-1 的增益刻度，latch 是主机收震后
 * 收音尾锁定的最后发声子帧（env 落到 0 时清除）。 */
export class CoilGate {
  constructor() {
    this.env = 0.0;
    this.latch = null;
  }
}

/** 两侧振荡器相位 + 扬声器相位/包络 + 各侧子帧游标与音圈包络门（跨块连续）。
 *
 * speaker 是 3kHz 蓝牙承载（发声段折进音圈）的声部，speakerBeat 是 0x36
 * 喇叭块专用声部——两路采样率不同、相位与包络不能混用。gates 是左右音圈的
 * 包络门，挂各自的子帧游标走。 */
export class VoiceState {
  constructor() {
    this.keyPhase = [[0.0, 0.0], [0.0, 0.0]];
    this.speaker = [0.0];
    this.speakerEnv = 0.0;
    this.speakerBeat = [0.0];
    this.speakerBeatEnv = 0.0;
    /** 每侧 [子帧序号, 距下次切换的样本数]。 */
    this.cursor = [[0, 0], [0, 0]];
    this.gates = [new CoilGate(), new CoilGate()];
  }
}

/** 发声段音色的哑渲染：基频 + 二次谐波，边沿触发起音/收音包络。
 * state.speaker（3kHz，折进音圈）或 state.speakerBeat（喇叭块）是跨块连续的
 * 相位与包络（幅度刻度）——增益从 0 变非 0 时按起音时长爬升，归零时按收音时长
 * 衰落；谐波频率越过奈奎斯特界限就只出基频。 */
export function renderSpeaker(tone, state, frames, rate, peak, beat = false) {
  const [freq, gain] = tone;
  const full = (gain * peak) / 255.0;
  const step = freq ? (TAU * freq) / rate : 0.0;
  const harm = freq && 2 * freq < rate / 2 ? SPEAKER_HARMONIC2 : 0.0;
  const attack = Math.max(1.0, SPEAKER_ATTACK_S * rate);
  const release = Math.max(1.0, SPEAKER_RELEASE_S * rate);
  let phase = beat ? state.speakerBeat[0] : state.speaker[0];
  let env = beat ? state.speakerBeatEnv : state.speakerEnv;
  const out = new Array(frames).fill(0);
  for (let i = 0; i < frames; i++) {
    if (full > 0.0) {
      env = Math.min(full, env + full / attack);
    } else {
      env = Math.max(0.0, env - peak / release);
    }
    if (env <= 0.0) {
      continue;
    }
    const value = (Math.sin(phase) + harm * Math.sin(phase * 2)) * SPEAKER_SHAPE;
    out[i] = clamp16(Math.round(value * env));
    phase = (phase + step) % TAU;
  }
  if (beat) {
    state.speakerBeat[0] = phase;
    state.speakerBeatEnv = env;
  } else {
    state.speaker[0] = phase;
    state.speakerEnv = env;
  }
  return out;
}

/** int16 刻度 → s8：饱和夹取（不回卷）。 */
export function toS8(value) {
  return Math.max(-128, Math.min(127, value));
}

/** 一侧的子帧序列里是否有非零增益的子帧。 */
export function sideActive(side) {
  const keys = side.keys ?? [];
  for (let i = 0; i < Math.min(keys.length, Math.max(0, side.count)); i++) {
    if (keys[i][0][1] || keys[i][1][1]) {
      return true;
    }
  }
  return false;
}

/** 参数里是否带要出的内容（任一侧音圈或发声段有增益）：发送线程的空闲唤醒按它判定。 */
export function hasContent(params) {
  const hd = params.hd;
  if (hd == null) {
    return false;
  }
  return sideActive(hd.l) || sideActive(hd.r) || Boolean(hd.speaker[1]);
}
