// WriteBackGate：桥接写回限速——蓝牙 HID 写回慢，写回风暴把会话循环拖到输入转发卡顿。
// 反馈帧打印限频：稳态每秒上百条 `反馈 震动 …`，窗口内只放行第一条。
import { describe, expect, it } from 'vitest';

import { FeedbackThrottle, WriteBackGate, formatFeedback } from '../src/session/gates.js';

function feedbackPayload(strength) {
  return Buffer.from([1, 1, strength, strength, 0x01, 0x00, strength, strength]);
}

describe('WriteBackGate', () => {
  it('burst is capped at interval', () => {
    // 震动包络逐包都变、设备侧去重压不住：0.5 秒内涌进 50 帧只放行
    // 窗口数（约 17 帧 @30ms），会话循环不再被逐条写回拖住。
    const gate = new WriteBackGate(0.03);
    let writes = 0;
    for (let i = 0; i < 50; i++) {
      if (gate.admit(Buffer.concat([Buffer.from([0x31, i % 256]), Buffer.alloc(76)]), i * 0.01)) {
        writes += 1;
      }
    }
    expect(writes).toBeLessThanOrEqual(18);
  });

  it('pending latest lands after window', () => {
    // 被窗口挡下的帧不丢，留作最新待写帧：停震的最后一帧（马达全零）
    // 必须在窗口到期后落地，不然手柄会被钉在震动上。
    const gate = new WriteBackGate(0.03);
    const stop = Buffer.concat([Buffer.from([0x31]), Buffer.alloc(77)]);
    expect(gate.admit(Buffer.concat([Buffer.from([0x31, 0x40]), Buffer.alloc(76)]), 0.0)).toBe(true);
    for (const t of [0.005, 0.01, 0.015, 0.02]) {
      expect(gate.admit(Buffer.concat([Buffer.from([0x31, 0x80]), Buffer.alloc(76)]), t)).toBe(false);
      expect(gate.admit(stop, t + 0.001)).toBe(false);
    }
    // 窗口内不放行，到期后 poll 放行的是最新一帧（停震帧）。
    expect(gate.poll(0.02)).toBeNull();
    expect(gate.poll(0.05)).toEqual(stop);
    expect(gate.poll(0.05)).toBeNull();
  });
});

describe('FeedbackThrottle', () => {
  it('first frame passes and burst merges with count', () => {
    const gate = new FeedbackThrottle(1.0);
    const first = gate.feed(feedbackPayload(15), 100.0);
    expect(first).toContain('强度 15/15');
    expect(first).not.toContain('合并');

    // 同一窗口内的后续帧全部合并，只计数不产生输出。
    for (let i = 0; i < 120; i++) {
      expect(gate.feed(feedbackPayload(9), 100.0 + i * 0.005)).toBeNull();
    }

    // 窗口过后下一条放行，并带上合并掉的条数。
    const merged = gate.feed(feedbackPayload(9), 102.0);
    expect(merged).not.toBeNull();
    expect(merged).toContain('已合并 120 条');
    expect(merged).toContain('强度 9/9');
  });

  it('merged counter resets after each release', () => {
    const gate = new FeedbackThrottle(1.0);
    gate.feed(feedbackPayload(15), 0.0);
    gate.feed(feedbackPayload(9), 0.1);
    gate.feed(feedbackPayload(9), 0.2);
    const merged = gate.feed(feedbackPayload(9), 1.5);
    expect(merged).toContain('已合并 2 条');
    // 释放后计数归零：再过窗口来的新帧不带合并前缀。
    const again = gate.feed(feedbackPayload(9), 3.0);
    expect(again).not.toContain('合并');
  });

  it('payload format includes high band', () => {
    const line = formatFeedback(feedbackPayload(9));
    expect(line).toContain('强度 9/9');
    expect(line).toContain('高频 9/9');
  });
});
