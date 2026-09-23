# XInput 形态手柄数据规范（Xbox 360 报文）

本规范记录说 XInput 报文（Xbox 360 形态）的手柄在 USB 下的输入报告布局与输出报告数据，
作为家族布局表（[pad/layouts/xinput.c](../firmware/main/pad/layouts/xinput.c)）与反馈编码的数据依据；
Xbox 家族的蓝牙 HID 报告见 [controller-xbox.md](controller-xbox.md)，Switch 一代见
[controller-ns1.md](controller-ns1.md)，PS 家族见 [controller-ps.md](controller-ps.md)。
字段偏移取自公开实现，落地前用 `pc/remapadctl.py --dump` 抓原始报告核对，核对状态见文末。

## 型号与标识

这份报文既是 Microsoft 的 Xbox 360 手柄（有线）的语言，也是大量第三方手柄在 XInput 形态下对外
报的格式：Logitech F310 / F510 / F710、8BitDo Ultimate 系列、GameSir、PDP / Afterglow、
Hori、PowerA、Nacon、Razer、Flydigi、CRKD 等。它们的厂商 VID 各不相同，因此 XInput 形态的家族
不按 VID 判定，而是按 `pad/layouts/xinput.c` 的型号表（取自公开实现的 XInput 型号清单）。

| 型号 | VID:PID 举例 | 备注 |
| :--- | :--- | :--- |
| Xbox 360 有线手柄 | `0x045E:0x028E`、`0x045E:0x028F` | 微软 VID，家族按 VID 即可判定 |
| Logitech F310 / F510 / F710 | `0x046D:0xC21D`、`0x046D:0xC21E`、`0x046D:0xC21F` | 切到 DirectInput 模式时 PID 与报文都不同 |
| 8BitDo Ultimate 系列 | `0x2DC8:0x3106`、`0x2DC8:0x3109`、`0x2DC8:0x310A`、`0x2DC8:0x310B` | 蓝牙下也报这段报文体 |
| GameSir T4 Kaleid / Nova 2 Lite | `0x3537:0x1004`、`0x3537:0x100F` | 同族的更多 PID 见布局文件的型号表 |

型号表按「公开实现里登记为 XInput 形态」的型号逐个列出，不按厂商 VID 一把抓：同一个厂商的
DirectInput 模式往往是另一个 PID、另一份报文，按 VID 判定会把两种报文混在一起。

## 输入报告布局

偏移一律从报告首字节起算（含 Report ID）。报文固定 20 字节，首两字节是报文类型与长度。

| 字段 | 偏移 | 长度 | 说明 |
| :--- | :--- | :--- | :--- |
| 报文类型 | `0x00` | 1 | 固定 `0x00` |
| 报文长度 | `0x01` | 1 | 固定 `0x14`（20）；部分第三方手柄不填这一字节，解析不看它 |
| 按键位图 | `0x02` | 2 | 低字节方向键与功能键、高字节肩键与面键 |
| 扳机 | `0x04` | 2 | LT / RT，单字节 0-255 |
| 左摇杆 | `0x06` | 4 | 两对有符号 16 位小端（X 在前），中位 0 |
| 右摇杆 | `0x0A` | 4 | 同上 |

设备 Y 轴向下为正（推满下是 `+32767`），解析侧翻成「上为正」。

### 按键位图

| 字节 | bit0 | bit1 | bit2 | bit3 | bit4 | bit5 | bit6 | bit7 |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `0x02` | 方向键上 | 方向键下 | 方向键左 | 方向键右 | Start（Menu） | Back（View） | L3 | R3 |
| `0x03` | LB | RB | 西瓜键 | — | A | B | X | Y |

面键按位置语义映射：物理 A（下）→ `PAD_BTN_CROSS`、物理 B（右）→ `PAD_BTN_CIRCLE`、
物理 X（左）→ `PAD_BTN_SQUARE`、物理 Y（上）→ `PAD_BTN_TRIANGLE`。
这份报文没有分享键，`0x03` 的 bit3 只作保留位。

## 输出报告（反馈）

| 偏移 | 字段 |
| :--- | :--- |
| `0x00` | 报文类型 `0x00` |
| `0x01` | `0x08`（震动报文长度） |
| `0x02` | `0x00` |
| `0x03` | 左大马达强度（低频带） |
| `0x04` | 右小马达强度（高频带） |
| `0x05`-`0x07` | `0x00` |

报文不带 Report ID：首字节就是报文自己的类型字节。玩家灯是另一份 3 字节报文
（`01 03 <档位>`，档位 2-5 依次是 1P-4P 的「闪烁后常亮」、6-9 是常亮），一份输出描述装不下
两份报文，布局行只声明震动这一份，灯不驱动。

无线接收器形态与有线共用这段报文体，只是前面多 4 字节前缀；接收器本身是厂商接口、不是 HID
手柄接口，本设备的 USB host 走 HID 报告，因此这份形态未登记，留待 PC 桥接核对。

## 核对状态与实测记录

| 数据 | 状态 |
| :--- | :--- |
| 输入报告的字段偏移与按键位 | 取自 Linux `xpad` 的 360 报文处理，未实机核对 |
| 型号表（哪些 VID:PID 说这份报文） | 取自公开实现的 XInput 型号清单，未逐个实机核对 |
| 输出报告 8 字节布局 | 取自 Linux `xpad` 的震动报文，未实机核对 |
| 玩家灯报文（3 字节）与无线接收器前缀 | 只作协议记录，未登记、未实机核对 |

## 参考资料

- Linux 内核 `drivers/input/joystick/xpad.c`：`xpad360_process_packet` 的字段偏移与按键位、
  `XTYPE_XBOX360` 型号表、震动与玩家灯输出报文的字节布局。
- free60 的 Gamepad 页面：Xbox 360 手柄的报告描述符（首两字节 `00 14`）。
