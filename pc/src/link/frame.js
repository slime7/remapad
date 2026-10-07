// Remapad 桥接帧编解码：帧格式与固件侧 input_frame.c 一致。
// 数据与核对状态见 docs/ABSTRACTIONS.md。

/** 帧同步字与版本；帧头 7 字节 + 载荷 + CRC16 两字节。 */
export const SYNC0 = 0xa5;
export const SYNC1 = 0x5a;
export const VERSION = 0x01;
export const HEADER_LEN = 7;
export const CRC_LEN = 2;
/** PC → 设备报文帧的载荷上限：8 字节设备标识 + 单帧最多 78 字节原始报告（DS5 蓝牙 0x31）。 */
export const MAX_PAYLOAD = 86;
export const MAX_FRAME = HEADER_LEN + MAX_PAYLOAD + CRC_LEN;
/** 设备 → PC 输出报告帧的载荷上限：DualSense / DualShock 4 蓝牙输出报告各 78 字节。 */
export const OUT_REPORT_MAX = 78;
/** 线格式上限：帧头里的长度字段是单字节，OTA 数据帧用到 202 字节。 */
export const WIRE_MAX_PAYLOAD = 255;
/** 解码器接受的单帧载荷上限：取线格式上限，放宽只是让截图分块这类长载荷帧能被收下。 */
export const DECODE_MAX_PAYLOAD = WIRE_MAX_PAYLOAD;
/** 截图声明载荷：宽 u16 LE + 高 u16 LE + 格式 u8。 */
export const IMAGE_INFO_LEN = 5;
/** 像素格式 1 = RGB565 小端：固件把渲染缓冲原样回传，不换字节序。 */
export const IMAGE_FORMAT_RGB565_LE = 1;
/** 截图分块载荷：偏移 u32 LE + 像素数据。 */
export const IMAGE_OFF_LEN = 4;

export const TYPE_ATTACH = 0x01;
export const TYPE_DETACH = 0x02;
export const TYPE_REPORT = 0x10;
/** 设备 → PC：要写回手柄的输出报告（原始字节，首字节是 Report ID）。 */
export const TYPE_OUT_REPORT = 0x11;
/** 设备 → PC：主机输出的原始采集（布局转换之前），固件默认关闭。 */
export const TYPE_HOST_RAW = 0x12;
export const TYPE_FEEDBACK = 0x20;
export const TYPE_IMAGE_INFO = 0x21;
export const TYPE_IMAGE_DATA = 0x22;
export const TYPE_IMAGE_END = 0x23;
export const TYPE_OTA_BEGIN = 0x30;
export const TYPE_OTA_DATA = 0x31;
export const TYPE_OTA_END = 0x32;
export const TYPE_OTA_ACK = 0x33;
/** amiibo 上传帧，载荷布局与固件 amiibo/amiibo_proto.h 一致：镜像固定 540 字节，逐帧回 ACK。 */
export const TYPE_AMIIBO_BEGIN = 0x40;
export const TYPE_AMIIBO_DATA = 0x41;
export const TYPE_AMIIBO_END = 0x42;
export const TYPE_AMIIBO_ACK = 0x43;
export const TYPE_PING = 0x7f;

/** BEGIN 载荷：magic + image_size(u32 LE)。 */
export const OTA_BEGIN_MAGIC = Buffer.from('ROM1');
/** DATA 载荷：seq(u16 LE) + 数据，单帧数据上限 200 字节。 */
export const OTA_DATA_MAX = 200;
/** ACK 载荷：state + code + next_seq(u16 LE) + received(u32 LE)。 */
export const OTA_ACK_LEN = 8;
/** BEGIN 的 ACK 在末尾追加的运行版本字段长度（ASCII，NUL 填充）。 */
export const OTA_ACK_VERSION_LEN = 16;
/** 一个窗口的帧数：设备每收满这么多帧回一次 ACK，PC 收到才发下一窗。 */
export const OTA_WINDOW_FRAMES = 16;
/** 数据帧的 slot 取 1 表示「这一帧是窗口的最后一帧」，设备收到即回应答。 */
export const OTA_SLOT_WINDOW_END = 1;

export const OTA_STATE_NAMES = { 0: 'idle', 1: 'receiving', 2: 'done', 3: 'failed' };
export const OTA_CODE_NAMES = {
  0: 'ok',
  1: '设备忙（已有升级在进行或镜像待验证）',
  2: '镜像头无效',
  3: '序号不连续',
  4: '写 flash 失败',
  5: '字节数与声明不符',
  6: '镜像校验失败',
  7: '设备侧超时',
};

/** BEGIN 载荷：name_len(u8) + name(UTF-8) + 镜像大小(u32 LE)。 */
export const AMIIBO_NAME_MAX = 31;
/** DATA 载荷：offset(u16 LE) + 数据，单帧数据上限 200 字节。 */
export const AMIIBO_DATA_MAX = 200;
/** NTAG215 用户区完整镜像（amiibo dump 通行尺寸）。 */
export const AMIIBO_TAG_SIZE = 540;
/** 厂商签名（READ_SIG 页）长度；572 字节 dump 把它附在镜像尾部。 */
export const AMIIBO_SIG_SIZE = 32;
/** 带签名的整份 dump（镜像 + 签名）。 */
export const AMIIBO_FULL_SIZE = AMIIBO_TAG_SIZE + AMIIBO_SIG_SIZE;
/** ACK 载荷：state + code + received(u32 LE) + slot（仅 done 有意义，0xFF 无）。 */
export const AMIIBO_ACK_LEN = 7;

export const AMIIBO_STATE_NAMES = { 0: 'idle', 1: 'receiving', 2: 'done', 3: 'failed' };
/** 数值与固件 amiibo_code_t 一致（测试与调用方按名字取用）。 */
export const AMIIBO_CODE_OK = 0;
export const AMIIBO_CODE_BUSY = 1;
export const AMIIBO_CODE_BAD_HEADER = 2;
export const AMIIBO_CODE_OFFSET_ERROR = 3;
export const AMIIBO_CODE_STORE_ERROR = 4;
export const AMIIBO_CODE_SIZE_MISMATCH = 5;
export const AMIIBO_CODE_TIMEOUT = 7;
export const AMIIBO_CODE_NAMES = {
  0: 'ok',
  1: '设备忙（已有上传在进行）',
  2: '名称或镜像大小无效',
  3: '数据偏移不衔接',
  4: '设备存储失败（槽位写满或 NVS 出错）',
  5: '字节数与声明不符',
  7: '设备侧超时',
};

export const FAMILY_UNKNOWN = 0;
export const FAMILY_XBOX = 1;
export const FAMILY_PS = 2;
export const FAMILY_STEAM = 3;

export const CONN_UNKNOWN = 0;
export const CONN_USB = 1;
export const CONN_BT = 2;

/** 已知厂商 → 家族。固件侧还会按同样的 VID 再判一次，这里只是随帧带过去的提示。 */
export const VENDOR_FAMILY = {
  0x045e: FAMILY_XBOX,
  0x054c: FAMILY_PS,
  0x28de: FAMILY_STEAM,
};

export const FAMILY_NAMES = {
  [FAMILY_UNKNOWN]: 'unknown',
  [FAMILY_XBOX]: 'xbox',
  [FAMILY_PS]: 'ps',
  [FAMILY_STEAM]: 'steam',
};

/** CRC-16/CCITT-FALSE（多项式 0x1021，初值 0xFFFF）。 */
export function crc16(data) {
  let crc = 0xffff;
  for (const byte of data) {
    crc ^= byte << 8;
    for (let i = 0; i < 8; i++) {
      crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
    }
  }
  return crc;
}

/** 编码一帧；载荷超过该帧类型允许的上限抛 RangeError。 */
export function encode(frameType, slot, seq, payload = Buffer.alloc(0), maxPayload = MAX_PAYLOAD) {
  if (payload.length > maxPayload) {
    throw new RangeError('载荷超过该帧类型允许的上限');
  }
  const frame = Buffer.alloc(HEADER_LEN + payload.length + CRC_LEN);
  frame[0] = SYNC0;
  frame[1] = SYNC1;
  frame[2] = VERSION;
  frame[3] = frameType;
  frame[4] = slot;
  frame[5] = seq;
  frame[6] = payload.length;
  payload.copy(frame, HEADER_LEN);
  const crc = crc16(frame.subarray(0, HEADER_LEN + payload.length));
  frame[frame.length - 2] = crc & 0xff;
  frame[frame.length - 1] = (crc >> 8) & 0xff;
  return frame;
}

/** BEGIN 载荷：magic + 镜像字节数（小端）。 */
export function otaBeginPayload(imageSize) {
  const payload = Buffer.alloc(8);
  OTA_BEGIN_MAGIC.copy(payload, 0);
  payload.writeUInt32LE(imageSize, 4);
  return payload;
}

/** DATA 载荷：块序号（小端）+ 镜像数据。 */
export function otaDataPayload(seq, chunk) {
  if (chunk.length > OTA_DATA_MAX) {
    throw new RangeError('OTA 数据块超过单帧上限');
  }
  const payload = Buffer.alloc(2 + chunk.length);
  payload.writeUInt16LE(seq, 0);
  chunk.copy(payload, 2);
  return payload;
}

/** 解析设备回发的 ACK；BEGIN 的应答末尾带 16 字节运行版本。 */
export function parseOtaAck(payload) {
  if (payload.length < OTA_ACK_LEN) {
    throw new TypeError(`ACK 载荷过短：${payload.length} 字节`);
  }
  let version = '';
  if (payload.length >= OTA_ACK_LEN + OTA_ACK_VERSION_LEN) {
    const raw = payload.subarray(OTA_ACK_LEN, OTA_ACK_LEN + OTA_ACK_VERSION_LEN);
    const end = raw.indexOf(0);
    version = raw.subarray(0, end < 0 ? OTA_ACK_VERSION_LEN : end).toString('utf8');
  }
  return {
    state: OTA_STATE_NAMES[payload[0]] ?? String(payload[0]),
    stateId: payload[0],
    code: OTA_CODE_NAMES[payload[1]] ?? String(payload[1]),
    codeId: payload[1],
    nextSeq: payload.readUInt16LE(2),
    received: payload.readUInt32LE(4),
    version,
  };
}

/** 解析截图声明：返回 {width, height, format}。 */
export function parseImageInfo(payload) {
  if (payload.length < IMAGE_INFO_LEN) {
    throw new TypeError(`截图声明过短：${payload.length} 字节`);
  }
  return { width: payload.readUInt16LE(0), height: payload.readUInt16LE(2), format: payload[4] };
}

/** 解析截图分块：返回 {offset, data}。 */
export function parseImageChunk(payload) {
  if (payload.length < IMAGE_OFF_LEN) {
    throw new TypeError(`截图分块过短：${payload.length} 字节`);
  }
  return { offset: payload.readUInt32LE(0), data: payload.subarray(IMAGE_OFF_LEN) };
}

/** 解析截图收尾：返回整幅画面的总字节数。 */
export function parseImageEnd(payload) {
  if (payload.length < 4) {
    throw new TypeError(`截图收尾过短：${payload.length} 字节`);
  }
  return payload.readUInt32LE(0);
}

/** 设备标识载荷：家族、连接方式、VID/PID 小端、Report ID 与报告长度。 */
export function deviceId(family, conn, vid, pid, reportId, reportLen) {
  return Buffer.from([
    family,
    conn,
    vid & 0xff,
    (vid >> 8) & 0xff,
    pid & 0xff,
    (pid >> 8) & 0xff,
    reportId,
    reportLen,
  ]);
}

export function familyForVendor(vid) {
  return VENDOR_FAMILY[vid] ?? FAMILY_UNKNOWN;
}

/** FEEDBACK 载荷里两带频率落地值（字节 8-15）的偏移；12 字节 = 老固件（无频率字段）。 */
export const FEEDBACK_FREQ_OFF = 8;
export const FEEDBACK_LEN = 16;
/** 57 字节版本再带 HD 时序子帧表（固件按布局行 hd 规则重整出的子帧序列，PC 侧哑渲染）。 */
export const FEEDBACK_HD_LEN = 57;
export const FEEDBACK_HD_KEY_MAX = 3;

function feedbackKeys(payload, base) {
  const count = payload[base];
  const keys = [];
  for (let k = 0; k < FEEDBACK_HD_KEY_MAX; k++) {
    const off = base + 1 + k * 6;
    const lf = payload[off] | (payload[off + 1] << 8);
    const lg = payload[off + 2];
    const hf = payload[off + 3] | (payload[off + 4] << 8);
    const hg = payload[off + 5];
    if (k < count) {
      keys.push([[lf, lg], [hf, hg]]);
    }
  }
  return { count, keys };
}

/** FEEDBACK 帧载荷 → 参数对象（音频触觉合成与打印共用）；载荷过短返回 null。 */
export function feedbackParams(payload) {
  if (payload.length < 8) {
    return null;
  }
  const params = {
    rumbleOn: [payload[0] !== 0, payload[1] !== 0],
    lfAmp: [payload[2], payload[3]],
    playerLed: payload[4],
    sample: payload[5],
    hfAmp: [payload[6], payload[7]],
    lfFreq: null,
    hfFreq: null,
    hd: null,
  };
  if (payload.length >= FEEDBACK_LEN) {
    params.lfFreq = [payload.readUInt16LE(FEEDBACK_FREQ_OFF), payload.readUInt16LE(FEEDBACK_FREQ_OFF + 2)];
    params.hfFreq = [payload.readUInt16LE(FEEDBACK_FREQ_OFF + 4), payload.readUInt16LE(FEEDBACK_FREQ_OFF + 6)];
  }
  if (payload.length >= FEEDBACK_HD_LEN) {
    params.hd = {
      l: feedbackKeys(payload, 16),
      r: feedbackKeys(payload, 35),
      speaker: [payload[54] | (payload[55] << 8), payload[56]],
    };
  }
  return params;
}

/** HOST_RAW 载荷头：通道字节 + 标志/长度字节（bit7 = 截断，低 7 位 = 数据长度）。 */
export const HOST_RAW_HEADER = 2;
/** 通道字节 → 名字（GATT 属性表句柄低字节，与固件 dp_capture.h 同一张表）。 */
export const HOST_RAW_CHANNELS = {
  0x05: 'base-config',
  0x12: 'rumble',
  0x14: 'cmd',
  0x16: 'composite',
  0x18: 'fwupg',
  0x22: 'ext-22',
  0x26: 'ext-26',
  0x2a: 'ext-2a',
  0x2c: 'ext-2c',
  0x2e: 'ext-2e',
  0x32: 'ext-32',
};

/** 采集帧载荷 → {channel, name, truncated, data}。 */
export function parseHostRaw(payload) {
  if (payload.length < HOST_RAW_HEADER) {
    throw new TypeError(`采集帧载荷过短：${payload.length} 字节`);
  }
  const channel = payload[0];
  const flags = payload[1];
  const length = flags & 0x7f;
  return {
    channel,
    name: HOST_RAW_CHANNELS[channel] ?? `ch-${channel.toString(16).padStart(2, '0')}`,
    truncated: (flags & 0x80) !== 0,
    data: payload.subarray(HOST_RAW_HEADER, HOST_RAW_HEADER + length),
  };
}

/** 与固件 input_frame_rx 同一套重新对齐规则：帧交给调用方，文本丢弃或回调。 */
export class FrameDecoder {
  constructor() {
    this._buf = Buffer.alloc(0);
  }

  /** 喂入字节，返回 {frames, text}；frames 元素为 {type, slot, seq, payload}。 */
  feed(data) {
    this._buf = this._buf.length ? Buffer.concat([this._buf, data]) : Buffer.from(data);
    const frames = [];
    const textChunks = [];
    let textLen = 0;
    for (;;) {
      const start = this._buf.indexOf(Buffer.from([SYNC0, SYNC1]));
      if (start < 0) {
        // 末字节可能是同步字前半，留到下一批。
        const keep = this._buf.length > 0 && this._buf[this._buf.length - 1] === SYNC0 ? 1 : 0;
        const drop = this._buf.length - keep;
        if (drop > 0) {
          textChunks.push(this._buf.subarray(0, drop));
          textLen += drop;
          this._buf = this._buf.subarray(drop);
        }
        break;
      }
      if (start > 0) {
        textChunks.push(this._buf.subarray(0, start));
        textLen += start;
        this._buf = this._buf.subarray(start);
      }
      if (this._buf.length < HEADER_LEN) {
        break;
      }
      const payloadLen = this._buf[6];
      if (payloadLen > DECODE_MAX_PAYLOAD) {
        this._buf = this._buf.subarray(1);
        continue;
      }
      const total = HEADER_LEN + payloadLen + CRC_LEN;
      if (this._buf.length < total) {
        break;
      }
      const want = this._buf[total - 2] | (this._buf[total - 1] << 8);
      if (crc16(this._buf.subarray(0, total - CRC_LEN)) !== want) {
        this._buf = this._buf.subarray(1);
        continue;
      }
      frames.push({
        type: this._buf[3],
        slot: this._buf[4],
        seq: this._buf[5],
        payload: Buffer.from(this._buf.subarray(HEADER_LEN, HEADER_LEN + payloadLen)),
      });
      this._buf = this._buf.subarray(total);
    }
    const text = Buffer.concat(textChunks, textLen);
    return { frames, text };
  }
}

/** BEGIN 载荷：名称长度 + 名称（UTF-8，1-31 字节）+ 镜像字节数（小端）。 */
export function amiiboBeginPayload(name, size) {
  const raw = Buffer.from(name, 'utf8');
  if (raw.length < 1 || raw.length > AMIIBO_NAME_MAX) {
    throw new RangeError(`amiibo 名称必须是 1-${AMIIBO_NAME_MAX} 字节（UTF-8），当前 ${raw.length}`);
  }
  const payload = Buffer.alloc(1 + raw.length + 4);
  payload[0] = raw.length;
  raw.copy(payload, 1);
  payload.writeUInt32LE(size, 1 + raw.length);
  return payload;
}

/** DATA 载荷：偏移（小端）+ 镜像数据。 */
export function amiiboDataPayload(offset, chunk) {
  if (chunk.length > AMIIBO_DATA_MAX) {
    throw new RangeError('amiibo 数据块超过单帧上限');
  }
  const payload = Buffer.alloc(2 + chunk.length);
  payload.writeUInt16LE(offset, 0);
  chunk.copy(payload, 2);
  return payload;
}

/** 解析设备回发的上传 ACK：state + code + received(u32 LE) + slot。 */
export function parseAmiiboAck(payload) {
  if (payload.length < AMIIBO_ACK_LEN) {
    throw new TypeError(`ACK 载荷过短：${payload.length} 字节`);
  }
  return {
    state: AMIIBO_STATE_NAMES[payload[0]] ?? String(payload[0]),
    stateId: payload[0],
    code: AMIIBO_CODE_NAMES[payload[1]] ?? String(payload[1]),
    codeId: payload[1],
    received: payload.readUInt32LE(2),
    slot: payload[6],
  };
}
