// USB-Serial/JTAG 串口：全仓库 PC 侧工具唯一的串口实现（转发、命令行、截图与 OTA 共用）。
// 直接用 Win32 API，并在打开前后把 DTR/RTS 固定为低电平——USB-Serial/JTAG 的
// 片内状态机把这两条线当复位控制线解释（RTS 拉高即复位），普通串口库默认会在
// 打开端口时拉起它们。
// 数据与核对状态见 docs/ABSTRACTIONS.md。

import koffi from 'koffi';

const GENERIC_READ = 0x80000000;
const GENERIC_WRITE = 0x40000000;
const OPEN_EXISTING = 3;
const PURGE_RXCLEAR = 0x0008;
const MAXDWORD = 0xffffffff;
const DTR_CONTROL_DISABLE = 0x00;
const RTS_CONTROL_DISABLE = 0x00;
const RTS_CONTROL_ENABLE = 0x01;

/** Win32 DCB：DTR/RTS 的状态就在 flags 位段里，打开前先定成禁用。 */
const Dcb = koffi.struct('_dcb_t', {
  DCBlength: 'uint32',
  BaudRate: 'uint32',
  flags: 'uint32',
  wReserved: 'uint16',
  XonLim: 'uint16',
  XoffLim: 'uint16',
  ByteSize: 'uint8',
  Parity: 'uint8',
  StopBits: 'uint8',
  XonChar: 'char',
  XoffChar: 'char',
  ErrorChar: 'char',
  EofChar: 'char',
  EvtChar: 'char',
  wReserved1: 'uint16',
});

const CommTimeouts = koffi.struct('_timeouts_t', {
  ReadIntervalTimeout: 'uint32',
  ReadTotalTimeoutMultiplier: 'uint32',
  ReadTotalTimeoutConstant: 'uint32',
  WriteTotalTimeoutMultiplier: 'uint32',
  WriteTotalTimeoutConstant: 'uint32',
});

const kernel32 = koffi.load('kernel32.dll');
const CreateFileW = kernel32.func('__stdcall', 'CreateFileW', 'int64', [
  'str16', 'uint32', 'uint32', 'void *', 'uint32', 'uint32', 'int64',
]);
const GetCommState = kernel32.func('__stdcall', 'GetCommState', 'int32', ['int64', koffi.inout(koffi.pointer(Dcb))]);
const SetCommState = kernel32.func('__stdcall', 'SetCommState', 'int32', ['int64', koffi.in(koffi.pointer(Dcb))]);
const SetCommTimeouts = kernel32.func('__stdcall', 'SetCommTimeouts', 'int32',
  ['int64', koffi.in(koffi.pointer(CommTimeouts))]);
const SetupComm = kernel32.func('__stdcall', 'SetupComm', 'int32', ['int64', 'uint32', 'uint32']);
const PurgeComm = kernel32.func('__stdcall', 'PurgeComm', 'int32', ['int64', 'uint32']);
const FlushFileBuffers = kernel32.func('__stdcall', 'FlushFileBuffers', 'int32', ['int64']);
const ReadFile = kernel32.func('__stdcall', 'ReadFile', 'int32', [
  'int64', 'uint8 *', 'uint32', koffi.out(koffi.pointer('uint32')), 'void *',
]);
const WriteFile = kernel32.func('__stdcall', 'WriteFile', 'int32', [
  'int64', 'uint8 *', 'uint32', koffi.out(koffi.pointer('uint32')), 'void *',
]);
const CloseHandle = kernel32.func('__stdcall', 'CloseHandle', 'int32', ['int64']);
const WinGetLastError = kernel32.func('__stdcall', 'GetLastError', 'uint32', []);

/** DCB.flags 位段偏移（fDtrControl 占 2 位、fRtsControl 占 2 位，其余各 1 位）。 */
const FLAG_BINARY = 1 << 0;
const FLAG_DTR_CONTROL_SHIFT = 4;
const FLAG_DSR_SENSITIVITY = 1 << 6;
const FLAG_RTS_CONTROL_SHIFT = 12;

const INVALID_HANDLE = -1n;

function fail(what) {
  // koffi 调用是同步的，紧跟着查 GetLastError 拿到的就是本次调用的错误码。
  const code = Number(WinGetLastError());
  const err = new Error(`${what}失败（Win32 错误 ${code}）`);
  err.code = code;
  return err;
}

/** 解码注册表里的 UTF-16LE 值：不同驱动登记的串口名有的带 NUL 结尾有的不带。 */
function decodeUtf16(buf, byteLen) {
  let text = buf.toString('utf16le', 0, Math.max(0, byteLen));
  let end = text.length;
  while (end > 0 && text.charCodeAt(end - 1) === 0) {
    end -= 1;
  }
  text = text.slice(0, end);
  return text;
}

/** 把打开端口失败映射成一句可读原因（图形界面直接显示这句）。 */
export function openHint(exc) {
  const code = Number(exc.code ?? exc.errno ?? 0);
  if (code === 5 || code === 32) {
    return '端口被占用，先结束占用进程（idf.py monitor、桥接程序等）';
  }
  if (code === 2) {
    return '端口不存在，确认设备已插好（Get-PnpDevice -Class Ports）';
  }
  return '打开端口失败';
}

/** COM 口按编号排序；不叫 COM<n> 的名字排在后面并按名字序。 */
export function portSortKey(name) {
  const digits = name.slice(3);
  return name.toUpperCase().startsWith('COM') && /^\d+$/.test(digits) ? ['', Number(digits)] : [name, 0];
}

/** 注册表里的 (设备名, 端口名) 列表 → 去重排序后的端口名列表。 */
export function serialPortNames(values) {
  const ports = new Set();
  for (const [_device, port] of values) {
    if (port.toUpperCase().startsWith('COM')) {
      ports.add(port);
    }
  }
  return [...ports].sort((a, b) => {
    const [groupA, numA] = portSortKey(a);
    const [groupB, numB] = portSortKey(b);
    if (groupA !== groupB) {
      return groupA < groupB ? -1 : 1;
    }
    return numA - numB;
  });
}

const advapi32 = koffi.load('advapi32.dll');
const HKEY_LOCAL_MACHINE = 0x80000002n;
const KEY_READ = 0x20019;
const RegOpenKeyExW = advapi32.func('__stdcall', 'RegOpenKeyExW', 'int32', [
  'int64', 'str16', 'uint32', 'uint32', koffi.out(koffi.pointer('int64')),
]);
const RegEnumValueW = advapi32.func('__stdcall', 'RegEnumValueW', 'int32', [
  'int64',
  'uint32',
  'uint8 *',
  koffi.inout(koffi.pointer('uint32')),
  'void *',
  koffi.out(koffi.pointer('uint32')),
  'uint8 *',
  koffi.inout(koffi.pointer('uint32')),
]);
const RegCloseKey = advapi32.func('__stdcall', 'RegCloseKey', 'int32', ['int64']);

/** 枚举本机串口：读注册表的 SERIALCOMM 键，读不到就返回空表。
 *
 * 命令行与图形界面都只用这份列表挑口（USB-Serial/JTAG、蓝牙调制解调器、
 * 虚拟串口都会出现，用哪一个是用户的选择）。
 */
export function listSerialPorts() {
  const keyOut = [0n];
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, 'HARDWARE\\DEVICEMAP\\SERIALCOMM', 0, KEY_READ, keyOut) !== 0) {
    return [];
  }
  const key = keyOut[0];
  try {
    const values = [];
    const nameBuf = Buffer.alloc(512);
    const dataBuf = Buffer.alloc(256);
    const typeOut = [0];
    for (let index = 0; ; index++) {
      const nameLen = [nameBuf.length / 2];
      const dataLen = [dataBuf.length];
      if (RegEnumValueW(key, index, nameBuf, nameLen, null, typeOut, dataBuf, dataLen) !== 0) {
        break;
      }
      const device = decodeUtf16(nameBuf, nameLen[0] * 2);
      const port = decodeUtf16(dataBuf, dataLen[0]);
      values.push([device, port]);
    }
    return serialPortNames(values);
  } finally {
    RegCloseKey(key);
  }
}

/** USB-Serial/JTAG 串口：打开与运行期间 DTR、RTS 始终为低。 */
export class SerialLink {
  constructor(port, baud = 115200, readTimeoutMs = 0) {
    this._handle = INVALID_HANDLE;
    this._rx = Buffer.alloc(0);
    // koffi 对放得进安全整数范围的 int64 返回 Number，统一归一成 BigInt 再比较。
    this._handle = BigInt(CreateFileW(`\\\\.\\${port}`, GENERIC_READ | GENERIC_WRITE, 0, null, OPEN_EXISTING, 0, 0n));
    if (this._handle === INVALID_HANDLE) {
      throw fail(`打开 ${port}`);
    }
    try {
      const dcb = {};
      if (!GetCommState(this._handle, dcb)) {
        throw fail('读取串口配置');
      }
      dcb.BaudRate = baud;
      dcb.flags = FLAG_BINARY
                | (DTR_CONTROL_DISABLE << FLAG_DTR_CONTROL_SHIFT)
                | (RTS_CONTROL_DISABLE << FLAG_RTS_CONTROL_SHIFT);
      dcb.ByteSize = 8;
      dcb.Parity = 0;
      dcb.StopBits = 0;
      if (!SetCommState(this._handle, dcb)) {
        throw fail('应用串口配置');
      }
      const timeouts = {
        ReadIntervalTimeout: MAXDWORD,
        ReadTotalTimeoutMultiplier: 0,
        ReadTotalTimeoutConstant: readTimeoutMs,
        WriteTotalTimeoutMultiplier: 0,
        WriteTotalTimeoutConstant: 1000,
      };
      if (!SetCommTimeouts(this._handle, timeouts)) {
        throw fail('设置串口超时');
      }
      SetupComm(this._handle, 4096, 4096);
      PurgeComm(this._handle, PURGE_RXCLEAR);
    } catch (err) {
      this.close();
      throw err;
    }
  }

  read(size = 4096) {
    const buf = Buffer.alloc(size);
    const got = [0];
    if (!ReadFile(this._handle, buf, size, got, null)) {
      throw fail('读取串口');
    }
    return buf.subarray(0, got[0]);
  }

  write(data) {
    const buf = Buffer.from(data);
    let sent = 0;
    while (sent < buf.length) {
      const chunk = buf.subarray(sent);
      const written = [0];
      if (!WriteFile(this._handle, chunk, chunk.length, written, null)) {
        throw fail('写入串口');
      }
      if (written[0] === 0) {
        throw new Error('写入串口失败：未接受任何字节');
      }
      sent += written[0];
    }
  }

  /** 返回一行（含换行）；没有完整行时返回手上的残行，完全没数据返回空。 */
  readline() {
    for (;;) {
      const end = this._rx.indexOf(0x0a);
      if (end >= 0) {
        const line = this._rx.subarray(0, end + 1);
        this._rx = this._rx.subarray(end + 1);
        return line;
      }
      const chunk = this.read();
      if (!chunk.length) {
        const line = this._rx;
        this._rx = Buffer.alloc(0);
        return line;
      }
      this._rx = this._rx.length ? Buffer.concat([this._rx, chunk]) : Buffer.from(chunk);
    }
  }

  /** 丢掉接收缓冲里还没读的数据（命令前后对齐用）。 */
  purgeInput() {
    this._rx = Buffer.alloc(0);
    PurgeComm(this._handle, PURGE_RXCLEAR);
  }

  /** 等待发送缓冲里的字节真正写出去。 */
  flush() {
    FlushFileBuffers(this._handle);
  }

  /** 硬复位：DTR 保持低，RTS 拉高 120 ms 再放下（esptool 的复位脉冲）。
     * sleep 可注入（测试用）；默认用 setTimeout，返回 Promise。
     */
  pulseReset(sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms))) {
    this._setRts(true);
    return sleep(120).then(() => {
      this._setRts(false);
      this.purgeInput();
    });
  }

  _setRts(level) {
    const dcb = {};
    if (!GetCommState(this._handle, dcb)) {
      this.close();
      throw fail('读取串口配置');
    }
    const control = level ? RTS_CONTROL_ENABLE : RTS_CONTROL_DISABLE;
    dcb.flags = (dcb.flags & ~(0x3 << FLAG_RTS_CONTROL_SHIFT)) | (control << FLAG_RTS_CONTROL_SHIFT);
    dcb.flags &= ~FLAG_DSR_SENSITIVITY;
    dcb.flags = (dcb.flags & ~(0x3 << FLAG_DTR_CONTROL_SHIFT)) | (DTR_CONTROL_DISABLE << FLAG_DTR_CONTROL_SHIFT);
    if (!SetCommState(this._handle, dcb)) {
      throw fail('设置 RTS');
    }
  }

  close() {
    if (this._handle !== INVALID_HANDLE) {
      CloseHandle(this._handle);
      this._handle = INVALID_HANDLE;
    }
  }
}
