# Remapad 桥接程序（PC → 设备）

把 PC 上插入的手柄转发给 Remapad：设备收到原始报告后按固件内的家族表解析、映射，
编码成 NS2 手柄报文经 BLE 发给主机。解析与映射只在固件里有一份，PC 侧只负责读手柄
与转发，屏幕上显示出来的按键位置与直插手柄将来自 USB host 路径时完全一致。

- `link.py`：桥接帧编解码（与固件 `firmware/main/input/input_frame.c` 同一套规则）
  与免复位的 Win32 串口打开。
- `bridge.py`：枚举手柄、按帧转发原始报告与设备标识、打印设备回发的反馈帧；
  `--dump` 只打印原始报告，用来核对固件家族表里的字段偏移。

## 依赖

```powershell
cd pc
uv sync
```

依赖由 [uv](https://docs.astral.sh/uv/) 管理：版本要求写在 `pyproject.toml`，锁文件是
`uv.lock`，环境建在 `pc/.venv`。第三方依赖只有 `hidapi` 一个（读手柄用），Python 需要
3.10 或更高；uv 找不到合适的解释器时会自己下载一个。串口部分直接调 Win32 API，不依赖
pyserial。当前实现只支持 Windows。

`uv run` 每次都会按锁文件把环境对齐，因此日常直接跑下面的命令即可；`uv sync` 只在想
显式建环境或核对依赖时用。

## 用法

以下命令都在 `pc/` 目录里执行（`uv run` 会使用 `pc/.venv`）：

```powershell
uv run python bridge.py --list                       # 列出候选的手柄接口
uv run python bridge.py --dump --seconds 10          # 采集 10 秒原始报告（核对布局用）
uv run python bridge.py -p COM3                      # 转发到设备
uv run python bridge.py -p COM3 --vid 0x054C --pid 0x0CE6
uv run python bridge.py -p COM3 --max-rate 250       # 限制转发帧率（0 表示不限制）
uv run python bridge.py -p COM3 --logs               # 同时打印设备日志文本
```

`--dump` 打印的每行是「时间戳 + 报告长度 + 原始字节」，用来与固件 `pad/pad_device.c`
里家族表的偏移对账：按住某个键只看一位变化，就能确认该键的字节与位序；摇杆推到极限
看量程与方向。对出来的偏移回填家族表，比在真机上猜「为什么按 A 出了 B」快得多。

## 与设备共用一个 COM 口

设备只有一根 Type-C：USB-Serial/JTAG 既跑固件日志与串口 CLI，也跑桥接帧。固件侧
`firmware/main/input/input_link.c` 是这条链路的唯一读取者，按帧头（`A5 5A`）把字节流
分成两类——桥接帧交给输入源，其余原样交给 CLI 行解析。因此桥接跑着的时候，
`python scripts/uartctl.py -p COM3 status` 依然可用。

打开这个口绝不能让设备复位：片内状态机把 DTR/RTS 当复位控制线解释（RTS 拉高即复位，
两条同时拉高会让设备停在不运行应用的状态）。`link.py` 用 Win32 API 打开端口并在打开
前后把两条线固定为低电平，与 [scripts/uartctl.py](../scripts/uartctl.py) 同一套做法；
自己写 PC 端工具时按同样规则处理。

## 桥接帧格式

```text
A5 5A | ver | type | slot | seq | len | payload[len] | crc16(LE)
```

CRC-16/CCITT-FALSE（多项式 `0x1021`、初值 `0xFFFF`）覆盖除末尾两字节外的整帧。载荷上限
72 字节；`REPORT` 帧的载荷是 8 字节设备标识（家族、连接方式、VID/PID 小端、Report ID、
报告长度）加上最多 64 字节原始报告。类型有 `ATTACH`（0x01）、`DETACH`（0x02）、
`REPORT`（0x10）、`FEEDBACK`（0x20，设备 → PC）、`PING`（0x7F）。

设备在主机下发 NS2 反馈（震动 / 玩家灯 / 触觉采样）时回发 `FEEDBACK` 帧，本轮 PC 侧只
打印；把反馈真正送到手柄在后续里程碑实现。

## 已知限制

- Xbox 有线、Xbox 蓝牙、PS 蓝牙与 Steam 原生布局的偏移初值取自公开资料，尚未逐条
  实机核对；核对前以 `--dump` 的结果为准，不符处回填 `pad/pad_device.c` 的家族表。
- 转发的是原始报告，不做任何按键重排：重排规则（用户自定义映射）在固件侧，本轮未做。
- 拔线或退出程序时发送 `DETACH` 帧，设备侧状态回到静置，不会留下卡住的按键。
