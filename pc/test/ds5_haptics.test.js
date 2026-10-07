// ds5_haptics 的哑渲染：时序子帧表（固件按布局行重整）直接驱动振荡器，
// 采样字节本身不进合成；老固件的两带参数回落成每侧两条同时叠加的正弦。
import { describe, expect, it } from 'vitest';

import {
  AMP_PEAK_BT,
  AMP_PEAK_USB,
  RATE,
  SPEAKER_RELEASE_S,
  hdVoices,
  legacyVoices,
  renderKeys,
  sliceSamples,
} from '../src/haptics/synth.js';
import { BT_RATE } from '../src/haptics/wire.js';
import { Ds5HapticsAudio } from '../src/haptics/audio.js';
import { silentSide } from './helpers/haptics.js';

function hdParams() {
  return {
    hd: {
      l: { count: 2, keys: [[[48, 255], [190, 64]], [[90, 2], [0, 0]]] },
      r: { count: 1, keys: [[[0, 0], [484, 128]]] },
      speaker: [880, 255],
    },
  };
}

describe('voices', () => {
  it('sample field does not enter synthesis', () => {
    // 触觉采样（0x0A 采样流）的原始 ID 只供日志展示：发声段由固件按
    // 音色表折成扬声器音色随 HD 段下发，PC 侧不消费采样字节。
    const audio = new Ds5HapticsAudio();
    audio.setParams({ sample: 0x02, lfAmp: [0, 0], hfAmp: [0, 0] });
    expect(hdVoices(audio._params)).toBeNull();
  });

  it('hd keys pass through', () => {
    // HD 子帧段原样进渲染：映射已在固件布局内完成，这里不做二次变换。
    const [left, right, speaker] = hdVoices(hdParams());
    expect(left.count).toBe(2);
    expect(right.count).toBe(1);
    expect(speaker).toEqual([[880, 255]]);
  });

  it('legacy bands fall back to two tones', () => {
    // 老固件 16 字节帧：两带振幅与频率落地值回落成每侧两条同时叠加的
    // 正弦，扬声器恒零（老固件不向桥接渲染发声段）。
    const params = { lfAmp: [64, 32], hfAmp: [16, 8], lfFreq: [55, 60], hfFreq: [190, 200] };
    const [left, right, speaker] = legacyVoices(params);
    expect(left).toEqual([[55.0, 64], [190.0, 16]]);
    expect(right).toEqual([[60.0, 32], [200.0, 8]]);
    expect(speaker).toEqual([]);
  });
});

describe('renderKeys', () => {
  it('silence with no keys', () => {
    // 没有子帧就是纯零：主机停震后触觉流立即安静。
    const out = renderKeys({ count: 0, keys: [] }, [0.0, 0.0], [0, 0], 32,
      BT_RATE, AMP_PEAK_USB, sliceSamples(BT_RATE));
    expect(out).toEqual(new Array(32).fill(0));
  });

  it('key produces waveform at scale', () => {
    // 满幅子帧扫过峰值刻度，零增益子帧不出声。
    const loud = renderKeys({ count: 3, keys: new Array(3).fill([[55, 255], [0, 0]]) },
      [0.0, 0.0], [0, 0], 480, RATE, AMP_PEAK_USB, sliceSamples(RATE));
    expect(Math.max(...loud.map(Math.abs))).toBeGreaterThan(20000);

    const silent = renderKeys({ count: 3, keys: new Array(3).fill([[55, 0], [0, 0]]) },
      [0.0, 0.0], [0, 0], 480, RATE, AMP_PEAK_USB, sliceSamples(RATE));
    expect(silent).toEqual(new Array(480).fill(0));
  });

  it('keys play in time order', () => {
    // 子帧按时间顺序轮播：强-静-弱的节奏在输出里按切片交替。
    const out = renderKeys({ count: 3, keys: [
      [[55, 255], [0, 0]], // 子帧 0：强
      [[0, 0], [0, 0]], // 子帧 1：静默
      [[55, 64], [0, 0]], // 子帧 2：弱
    ] }, [0.0, 0.0], [0, 0], 45, BT_RATE, AMP_PEAK_USB, sliceSamples(BT_RATE));
    // 3kHz 下每个切片 15 样本：0-14 强、15-29 静、30-44 弱。
    expect(Math.max(...out.slice(0, 15).map(Math.abs))).toBeGreaterThan(6000);
    expect(out.slice(15, 30)).toEqual(new Array(15).fill(0));
    expect(Math.max(...out.slice(30).map(Math.abs))).toBeGreaterThan(0);
    expect(Math.max(...out.slice(30).map(Math.abs)))
      .toBeLessThan(Math.max(...out.slice(0, 15).map(Math.abs)));
  });

  it('single declared key stays continuous', () => {
    // 声明 1 个子帧的持续震动连续：主机是 200Hz 的单子帧流，声明之外的
    // 槽位不占时间——固定按 3 槽轮播会把持续震动切成 66Hz 断续。
    const side = { count: 1, keys: [[[135, 255], [0, 0]]] };
    const phases = [0.0, 0.0];
    const cursor = [0, 0];
    const sliceSize = sliceSamples(BT_RATE);
    for (let i = 0; i < 4; i++) {
      const out = renderKeys(side, phases, cursor, 32, BT_RATE, AMP_PEAK_BT, sliceSize);
      expect(Math.max(...out.map(Math.abs))).toBeGreaterThan(100);
    }
  });

  it('phase carries across blocks', () => {
    // 相位跨块推进：两块拼接处不重置。三个子帧同频同幅：切片边界不应产生任何跳变。
    const side = { count: 3, keys: new Array(3).fill([[190, 200], [0, 0]]) };
    const phases = [0.0, 0.0];
    const cursor = [0, 0];
    const sliceSize = sliceSamples(RATE);
    const first = renderKeys(side, phases, cursor, 96, RATE, AMP_PEAK_USB, sliceSize);
    const second = renderKeys(side, phases, cursor, 96, RATE, AMP_PEAK_USB, sliceSize);
    // 相邻样本差值上界 ≈ 18800×2π×190/48000 ≈ 463，留裕量到 2000。
    expect(Math.abs(second[0] - first.at(-1))).toBeLessThanOrEqual(2000);
  });
});

describe('Ds5HapticsAudio block layout', () => {
  it('callback block layout', () => {
    // WASAPI 整块字节：4ch 交错 int16，扬声器两路放发声音色，触觉两路各跟
    // 各的子帧、发声段同时折进音圈（与蓝牙通路一致）。
    const audio = new Ds5HapticsAudio();
    audio.setParams(hdParams());
    const frames = 480;
    const block = audio.renderBlock(frames);
    const ch1 = [];
    const ch3 = [];
    const ch4 = [];
    for (let i = 0; i < frames; i++) {
      ch1.push(block.readInt16LE(i * 8));
      ch3.push(block.readInt16LE(i * 8 + 4));
      ch4.push(block.readInt16LE(i * 8 + 6));
    }
    expect(Math.max(...ch1.map(Math.abs))).toBeGreaterThan(10000); // 发声段铺频道 1/2
    expect(Math.max(...ch3.map(Math.abs))).toBeGreaterThan(10000); // 左音圈
    // 右侧子帧 0 高频 484Hz：声明 1 个子帧就是整段都在播它（折进来的发声
    // 段叠在上面），第二半块因此不是纯扬声器音色。
    expect(Math.max(...ch4.map(Math.abs))).toBeGreaterThan(3000);
    expect(Math.max(...ch4.slice(240).map(Math.abs))).toBeGreaterThan(3000);
    expect(ch4.slice(240)).not.toEqual(ch1.slice(240));
  });

  it('speaker segment reaches coil channels', () => {
    // 发声段折进两侧音圈：子帧全静、只有扬声器音色时，触觉两路与扬声器
    // 两路同样在响。
    const audio = new Ds5HapticsAudio();
    audio.setParams({
      hd: { l: silentSide(), r: silentSide(), speaker: [500, 255] },
    });
    const frames = 480;
    const block = audio.renderBlock(frames);
    const ch1 = [];
    const ch2 = [];
    const ch3 = [];
    const ch4 = [];
    for (let i = 0; i < frames; i++) {
      ch1.push(block.readInt16LE(i * 8));
      ch2.push(block.readInt16LE(i * 8 + 2));
      ch3.push(block.readInt16LE(i * 8 + 4));
      ch4.push(block.readInt16LE(i * 8 + 6));
    }
    expect(Math.max(...ch1.map(Math.abs))).toBeGreaterThan(10000);
    expect(ch3).toEqual(ch1);
    expect(ch4).toEqual(ch2);
  });

  it('legacy callback keeps speaker silent', () => {
    // 老固件两带参数：扬声器两路恒零。
    const audio = new Ds5HapticsAudio();
    audio.setParams({ lfAmp: [255, 0], hfAmp: [0, 0], lfFreq: [48, 48], hfFreq: [190, 190] });
    const frames = 480;
    const block = audio.renderBlock(frames);
    const ch1 = [];
    const ch3 = [];
    for (let i = 0; i < frames; i++) {
      ch1.push(block.readInt16LE(i * 8));
      ch3.push(block.readInt16LE(i * 8 + 4));
    }
    expect(Math.max(...ch1.map(Math.abs))).toBe(0);
    expect(Math.max(...ch3.map(Math.abs))).toBeGreaterThan(10000);
  });

  it('speaker tone fades in and out', () => {
    // 发声段带起音/收音包络：起播第一个样本远小于满幅，包络在起音时长内
    // 爬到满幅；增益归零后收音尾平滑落回静音。
    const audio = new Ds5HapticsAudio();
    audio.setParams({ hd: { l: silentSide(), r: silentSide(), speaker: [880, 255] } });
    const frames = 960;
    let ch1 = [];
    let block = audio.renderBlock(frames);
    for (let i = 0; i < frames; i++) {
      ch1.push(block.readInt16LE(i * 8));
    }
    expect(Math.abs(ch1[0])).toBeLessThan(4000); // 起音：第一拍不是满幅硬切
    expect(Math.max(...ch1.map(Math.abs))).toBeGreaterThan(18000); // 包络内爬到满幅

    audio.setParams({ hd: { l: silentSide(), r: silentSide(), speaker: [0, 0] } });
    block = audio.renderBlock(frames);
    ch1 = [];
    for (let i = 0; i < frames; i++) {
      ch1.push(block.readInt16LE(i * 8));
    }
    // 收音尾：起始处还有声，收音时长过后落回静音。
    expect(Math.abs(ch1[0])).toBeGreaterThan(1000);
    const release = Math.round(SPEAKER_RELEASE_S * RATE);
    expect(Math.max(...ch1.slice(release + 1).map(Math.abs))).toBe(0);
  });

  it('coil rumble fades after host stops', () => {
    // 主机收震后音圈走收音包络而不是块对齐硬切：与蓝牙 0x32 通路同一份门控行为。
    const audio = new Ds5HapticsAudio();
    const silent = silentSide();
    audio.setParams({ hd: { l: { count: 1, keys: [[[55, 255], [0, 0]]] }, r: silent, speaker: [0, 0] } });
    const frames = 480;
    let block = audio.renderBlock(frames);
    const ch3 = [];
    for (let i = 0; i < frames; i++) {
      ch3.push(block.readInt16LE(i * 8 + 4));
    }
    expect(Math.max(...ch3.map(Math.abs))).toBeGreaterThan(10000);

    audio.setParams({ hd: { l: silent, r: silent, speaker: [0, 0] } });
    const tails = [];
    for (let round = 0; round < 3; round++) {
      block = audio.renderBlock(frames);
      const tail = [];
      for (let i = 0; i < frames; i++) {
        tail.push(block.readInt16LE(i * 8 + 4));
      }
      tails.push(tail);
    }
    expect(Math.abs(tails[0][0])).toBeGreaterThan(0); // 收震后第一块仍在收音尾
    expect(Math.max(...tails[2].map(Math.abs))).toBe(0); // 尾长过后全静
  });
});
