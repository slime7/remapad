// DS5 音频触觉的 PC 侧统一出口：合成、报文、编码器与两条承载通路。
export * from './synth.js';
export * from './wire.js';
export { Bt36OpusEncoder } from './encoder.js';
export { Ds5HapticsAudio } from './audio.js';
export { Ds5HapticsBt } from './bt.js';
