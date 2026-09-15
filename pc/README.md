# Remapad 桥接程序（PC → 设备）

把 PC 上插入的手柄转发给 Remapad：设备收到原始报告后按固件内的家族表解析、映射，
编码成 NS2 手柄报文经 BLE 发给主机。解析与映射只在固件里有一份，PC 侧只负责读手柄
与转发，屏幕上显示出来的按键位置与直插手柄将来自 USB host 路径时完全一致。

- `link.py`：桥接帧编解码（与固件 `firmware/main/input/input_frame.c` 同一套规则）
  与免复位的 Win32 串口打开；三个工具共用这一份实现。
- `bridge.py`：枚举手柄、按帧转发原始报告与设备标识、打印设备回发的反馈帧；
  `--dump` 只打印原始报告，用来核对固件家族表里的字段偏移。
- `uartctl.py`：串口行命令客户端（固件 CLI 的 PC 端），命令清单见
  [GETTING-STARTED.md](../docs/GETTING-STARTED.md)。
- `ota.py`：把固件应用镜像推给设备做 OTA 升级，协议与回滚门槛见
  [ADR 0022](../docs/adr/0022-ota-over-bridge-frames-with-rollback.md)。

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
uv run python bridge.py -p COM3 --no-rumble          # 不把主机的震动/玩家灯写回手柄
```

`--dump` 打印的每行是「时间戳 + 报告长度 + 原始字节」，用来与固件 `pad/layouts/` 里
对应系列的偏移对账：按住某个键只看一位变化，就能确认该键的字节与位序；摇杆推到极限
看量程与方向。对出来的偏移回填该系列的布局文件，比在真机上猜「为什么按 A 出了 B」快得多。

## 与设备共用一个 COM 口

设备只有一根 Type-C：USB-Serial/JTAG 既跑固件日志与串口 CLI，也跑桥接帧。固件侧
`firmware/main/input/input_link.c` 是这条链路的唯一读取者，按帧头（`A5 5A`）把字节流
分成两类——桥接帧交给输入源，其余原样交给 CLI 行解析。因此桥接跑着的时候，
`uv run python uartctl.py -p COM3 status` 依然可用。

打开这个口绝不能让设备复位：片内状态机把 DTR/RTS 当复位控制线解释（RTS 拉高即复位，
两条同时拉高会让设备停在不运行应用的状态）。`link.py` 用 Win32 API 打开端口并在打开
前后把两条线固定为低电平；自己写 PC 端工具时按同样规则处理。

## 桥接帧格式

```text
A5 5A | ver | type | slot | seq | len | payload[len] | crc16(LE)
```

CRC-16/CCITT-FALSE（多项式 `0x1021`、初值 `0xFFFF`）覆盖除末尾两字节外的整帧。帧头里的
长度是单字节（线格式上限 255 字节）；`REPORT` 帧的载荷是 8 字节设备标识（家族、连接方式、
VID/PID 小端、Report ID、报告长度）加上最多 64 字节原始报告，因此报文帧按 72 字节校验。
类型有 `ATTACH`（0x01）、`DETACH`（0x02）、`REPORT`（0x10）、`OUT_REPORT`（0x11，设备 → PC）、`FEEDBACK`（0x20，设备 → PC）
与 `PING`（0x7F），另有 OTA 升级用的 `OTA_BEGIN`（0x30）、`OTA_DATA`（0x31，载荷到 202 字节）、
`OTA_END`（0x32）与设备回发的 `OTA_ACK`（0x33）。

设备在主机下发 NS2 反馈（震动 / 玩家灯 / 触觉采样）时回发两种帧：
`FEEDBACK` 是归一化状态（打印与对账用），`OUT_REPORT` 是已经编码好的手柄输出报告——震动与玩家灯的字段布局只在固件里有一份（`firmware/main/pad/feedback.c` 按设备布局行编码）。
PC 侧只把它交给 `hid.write()`，不参与任何映射（PS 系蓝牙形态的帧头与尾部 CRC32 也由固件算好）。输出报告的字节数按设备布局行的 `out` 描述来，
最长的两行是 DualSense 与 DualShock 4 的蓝牙形态（78 字节），因此这一帧按 78 字节校验。
默认开启，`--no-rumble` 关掉。

## OTA 升级（ota.py）

```powershell
uv run python ota.py --dry-run                 # 只校验镜像，不接设备
uv run python ota.py -p COM3                   # 升级默认镜像 ../firmware/build/remapad_firmware.bin
uv run python ota.py -p COM3 --wait            # 升级后等设备重启回来并打印版本
uv run python ota.py -p COM3 --verbose         # 同时透传设备日志
```

上传前先在本地校验镜像：首字节 `0xE9`、芯片标识 `0x0009`（ESP32-S3）、偏移 `0x20` 的应用
描述符（项目名必须是 `remapad_firmware`、版本取自构建时的 `git describe`）与 4 MB 分区上限。
上传按 16 帧一个窗口推送，收到设备 ACK（含期望序号与已收字节）才发下一窗；ACK 的期望序号
就是重发起点，因此超时重发不会重复写 flash。窗口末帧在帧头 `slot` 上带标记（末尾不足一窗也带），
设备收到即应答，不必等固定帧数；整窗重发时设备的序号错误应答按 50 ms 最小间隔限流，
既不会被重复应答干扰，某次应答被设备日志挤掉后重问也仍拿得到。

设备侧行为与恢复路径见 [GETTING-STARTED.md](../docs/GETTING-STARTED.md) 的「固件 OTA 升级」。

## 已知限制

- 家族表里的偏移多数取自公开资料，尚未逐条实机核对（只有 DualSense 蓝牙的 0x31
  行按 DualSense Edge 实测核对过）；核对前以 `--dump` 的结果为准，不符处回填
  `pad/layouts/` 下对应系列的文件。PS 系按 PID 分行（DS3、DS4、DualSense 有线都报 0x01），
  `--list` 的型号与连接方式可用于判断命中了哪一行。
- 转发的是原始报告，不做任何按键重排：重排规则（用户自定义映射）在固件侧，本轮未做。
- 拔线或退出程序时发送 `DETACH` 帧，设备侧状态回到静置，不会留下卡住的按键。
- 反馈写回依赖固件里的输出报告描述（DS4 / DualSense / Xbox / DS3 / NS1 各一行），这些描述多数取自公开资料：
  DualSense 蓝牙（0x31）行已按 DualSense Edge 实机核对——帧头 `00 10` 加尾部 CRC32，缺 CRC 时手柄整份报告都不接受（写回却照样返回成功）；
  蓝牙上主机自己的连接动画会一直盖着灯，灯条设置（`valid_flag2` + `lightbar_setup`）必须与颜色写在同一帧里才压得住，玩家灯按五颗灯的模式表点亮。
  其余行同样未核对；写回没效果时先看该系列布局行的 `out` 描述。
- 手柄同时按住 L1+R1+L3+R3（约 300 ms）会被设备捕获成屏幕操控模式：设备先补一帧全松开、其后续发中性帧（玩家的按键不再上行），之后方向键移动屏幕焦点、圆圈键等价于点按屏幕。
  再按一次同样的组合退出（[ADR 0028](../docs/adr/0028-pad-combo-captures-screen.md)）。桥接程序不感知这个状态，转发照旧；
  不想要这个行为就别按这个组合，串口 `ui on` / `ui off` 可以直接置位验证。
