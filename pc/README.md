# Remapad PC 侧工具（remapadctl）

`remapadctl.py` 是 PC 侧唯一入口：一个进程同时做四件事——把手柄原始报告转发给设备、
当串口命令行、抓实机截图、推固件 OTA。设备只有一根 Type-C，USB-Serial/JTAG 既跑
桥接帧也跑固件日志与 CLI 文本，因此这些能力共用同一个串口句柄，彼此不再抢口。

- `link.py`：桥接帧编解码（与固件 `firmware/main/input/input_frame.c` 同一套规则）
  与免复位的 Win32 串口打开，是 PC 侧唯一的串口实现。
- `remapadctl.py`：桥接转发（读手柄、发桥接帧、把主机的震动与玩家灯写回手柄）、
  串口命令行、实机截图、固件 OTA 与交互式工具命令。

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
uv run python remapadctl.py --list                    # 列出候选的手柄接口
uv run python remapadctl.py --dump --seconds 10       # 采集 10 秒原始报告（核对布局用）
uv run python remapadctl.py -p COM3                   # 桥接 + 交互命令行
uv run python remapadctl.py -p COM3 --no-pad          # 只当串口命令行用，不转发手柄
uv run python remapadctl.py -p COM3 status            # 执行一条设备命令后退出
uv run python remapadctl.py -p COM3 --all             # 拉取设备全部观测数据后退出
uv run python remapadctl.py -p COM3 --shot            # 实机截图存成 PNG
uv run python remapadctl.py -p COM3 --log --seconds 20
uv run python remapadctl.py -p COM3 --log --reset --seconds 25
uv run python remapadctl.py -p COM3 --upgrade --wait
uv run python remapadctl.py -p COM3 --vid 0x054C --pid 0x0CE6 --max-rate 250 --no-rumble
uv run python remapadctl.py -p COM3 --logs            # 桥接的同时打印设备日志
```

`--all` 把设备的全部观测命令各发一遍（status / mem / version / link / pad / usb /
report / ui / adv / headset / fwver / fwack / fwpost / fwapply / ctrl / backlight /
screen / relay / motion / ltk / rumble / lamp / haptic），数据全部由固件现场读取——
不经过 UI 层，UI 冻结（截图期间、页面门控不取数）不影响实时性。

## 串口命令面（完整控制手柄）

交互模式里不是 `:` 开头的行按固件 CLI 原样发送，手柄功能的完整控制面都在固件 CLI 里
（设备侧敲 `help` 有全表）：

```text
key a 200        注入按键（a b x y plus minus home capture c l r zl zr ls rs
                 up down left right gl gr ui），key release 全部松开
stick l 2048 2048  设摇杆电平 0-4095（stick reset 回中）
ctrl joycon      手柄形态与配色（ctrl pro 0x2d2d2d 0x8b0000 0x2d2d2d），持久化
connect          连接键：开连接窗口等主机连上来（未配对身份进配对流程）
pairing start    配新主机：断链 + 发现广播；pairing stop 停止广播并断链
wake             打开唤醒窗口；adv auto|wake|reconnect 钉窗口内形态
drop             断开当前主机
motion 3         0x09 运动块内容（0 全零 / 1 抓包占位 / 2 不带 / 3 真实样本）
headset 0x05     耳机状态字节（auto 回到按输入设备派生）
fwver 9.9.9      上报给主机的手柄固件版本；fwpost / fwack / fwapply 配套假升级
ltk 1            LTK 存储形态；relay 1 同代透传开关
rumble 200 0     手动震动（0-255 双侧，rumble off 停）；lamp 0xF 玩家灯；
                 haptic 0x10 触觉采样——与主机反馈走同一条编码投递路径
ui on            手柄操控屏幕模式（ui off 退出）
backlight 60     背光（持久化）；screen off 息屏；beep 蜂鸣；mode host USB 角色
```

无参敲这些命令即回读当前值（`backlight`、`ctrl`、`motion`、`relay`、`rumble`…），
`--all` 拉的就是这批回读。`mem` 的应答在下一帧打出：PSRAM / 内部堆的余量与历史
最低、QuickJS 记账与对象计数，观察内存趋势不用等 60 秒一条的周期日志。

交互模式里 `:` 开头的是本工具命令：

```text
:help              显示工具命令清单
:all               拉取设备全部观测数据（同 --all）
:shot [路径]       抓实机截图并存成 PNG
:log [秒|off]      透传设备日志（0 表示持续到 :log off）
:ota [镜像路径]    推固件镜像（默认 ../firmware/build/remapad_firmware.bin）
:quit              退出
```

手柄转发默认只在交互模式里开：一次性命令、`--shot`、`--log` 与 `--upgrade` 都不碰手柄（否则
主机会看到手柄闪一下），要在这些模式里也转发就加 `--pad`；`--no-pad` 在任何模式下都关掉转发。

`--dump` 打印的每行是「时间戳 + 报告长度 + 原始字节」，用来与固件 `pad/layouts/` 里
对应系列的偏移对账：按住某个键只看一位变化，就能确认该键的字节与位序；摇杆推到极限
看量程与方向。对出来的偏移回填该系列的布局文件，比在真机上猜「为什么按 A 出了 B」快得多。

## 会话与设备共用一根 Type-C

设备只有一根 Type-C：USB-Serial/JTAG 既跑固件日志与串口 CLI，也跑桥接帧。固件侧
`firmware/main/input/input_link.c` 是这条链路的唯一读取者，按帧头（`A5 5A`）把字节流
分成两类——桥接帧交给输入源，其余原样交给 CLI 行解析。PC 侧同样只有一个持有者：
`remapadctl.py` 一个进程同时收发，因此桥接运行期间命令、截图与升级都照常可用。

打开这个口绝不能让设备复位：片内状态机把 DTR/RTS 当复位控制线解释（RTS 拉高即复位，
两条同时拉高会让设备停在不运行应用的状态）。`link.py` 用 Win32 API 打开端口并在打开
前后把两条线固定为低电平；自己写 PC 端工具时按同样规则处理。

## 实机截图（--shot / :shot）

固件侧的串口 `shot` 命令只置一个标志：PocketJS owner task 在下一帧把当前画面按整屏
重渲染一遍（与面板上看到的是同一条渲染路径，含本机渲染加速器），再按 200 字节分块经
图像帧回传。PC 侧按偏移把分块拼齐，写进 `pc/shots/remapad-<时间戳>.png`（`--out` 可
指定路径）；缺块、越界或超时都不写文件，只报一行原因。

截图期间屏幕会整屏刷一次、UI 冻结约 0.2–1 秒：这是调试通路，BLE 输入在另一个任务上，
不受影响。

## 3.5mm 耳机状态

固件把输入设备报告里的耳机状态经 NS2 报文报给主机（`0x09` 偏移 `0x0D` 与 `0x05` 的耳机
插入位）。2026-09-15 在 DualSense Edge（`0x0DF2`，蓝牙 `0x31`）上按插拔差分核对完毕：
**第 55 字节** bit0 是插入、bit1 是带麦，插 → 拔 → 插得到 `0x03` → `0x00` → `0x01` →
`0x03`（第 56 字节跟着 bit0 走）。该行已登记 `headset_off = 55` 与
`PAD_HEADSET_PS`；DS4 与 DS5 有线行的耳机字节还没有抓包，保持不解析。

主机侧同一晚做过取值 A/B（`headset <值>` 后 `wake` 重建会话，用 `link` 看 `notify=`）：

| `0x0D` 取值 | 主机反应 |
| :--- | :--- |
| `0x00` 未插入 / `0x05` 插入 / `0x0D` 插入另一档 | 保持 `0x000E` 订阅，按上报节奏收帧（15 ms 下 66 帧/秒、`txf=0`） |
| `0x07` / `0x0F` 带麦 | 订阅后约 150 ms 取消订阅，输入不再被采用 |

所以 auto 派生值只报「插入」（`0x05`），带麦位不上行：主机认那一档的前提是 `0x002C`
上的音频 / 麦克风通路，本轮不做。要复现这组对照：`headset 0x07`（或 `0x0F`）→ `wake`
→ `link` 看 `notify=--`；`headset auto` 回到派生值。

换手柄或换系列时按同样步骤复核：

1. 手柄插在 PC 上并连上，跑 `uv run python remapadctl.py --dump --seconds 45`；
2. 期间把 3.5mm 耳机插 → 拔 → 插，每段约 8 秒；
3. 找唯一跟着变化的字节，填进对应布局行的 `headset_off` 并置 `.headset_style = PAD_HEADSET_PS`；
4. 重新编译烧录后，主机侧的耳机指示应跟着插拔变化（`headset auto`）。

## 桥接帧格式

```text
A5 5A | ver | type | slot | seq | len | payload[len] | crc16(LE)
```

CRC-16/CCITT-FALSE（多项式 `0x1021`、初值 `0xFFFF`）覆盖除末尾两字节外的整帧。帧头里的
长度是单字节（线格式上限 255 字节）；`REPORT` 帧的载荷是 8 字节设备标识（家族、连接方式、
VID/PID 小端、Report ID、报告长度）加上最多 64 字节原始报告，因此报文帧按 72 字节校验。
类型有 `ATTACH`（0x01）、`DETACH`（0x02）、`REPORT`（0x10）、`OUT_REPORT`（0x11，设备 → PC）、
`FEEDBACK`（0x20，设备 → PC）、截图帧 `IMAGE_INFO` / `IMAGE_DATA` / `IMAGE_END`（0x21-0x23，设备 → PC）、
OTA 升级用的 `OTA_BEGIN`（0x30）、`OTA_DATA`（0x31，载荷到 202 字节）、`OTA_END`（0x32）、
设备回发的 `OTA_ACK`（0x33），以及 `PING`（0x7F）。

截图三帧的载荷：`IMAGE_INFO` 是宽 u16 小端 + 高 u16 小端 + 格式（1 = RGB565 小端）共 5 字节；
`IMAGE_DATA` 是整幅画面的字节偏移 u32 小端 + 最多 200 字节像素；`IMAGE_END` 是总字节数 u32 小端。
完整性由偏移覆盖满整幅画面判定，不再另做校验和。

设备在主机下发 NS2 反馈（震动 / 玩家灯 / 触觉采样）时回发两种帧：
`FEEDBACK` 是归一化状态（打印与对账用），`OUT_REPORT` 是已经编码好的手柄输出报告——
震动与玩家灯的字段布局只在固件里有一份（`firmware/main/pad/feedback.c` 按设备布局行编码）。
PC 侧只把它交给 `hid.write()`，不参与任何映射（PS 系蓝牙形态的帧头与尾部 CRC32 也由固件算好）。
输出报告的字节数按设备布局行的 `out` 描述来，最长的两行是 DualSense 与 DualShock 4 的蓝牙形态
（78 字节），因此这一帧按 78 字节校验。默认开启，`--no-rumble` 关掉。

## 固件 OTA（--upgrade）

```powershell
uv run python remapadctl.py --dry-run                     # 只校验镜像，不接设备
uv run python remapadctl.py -p COM3 --upgrade             # 升级默认镜像 firmware/build/remapad_firmware.bin
uv run python remapadctl.py -p COM3 --upgrade --wait      # 等设备重启回来并打印版本
uv run python remapadctl.py -p COM3 --upgrade --verbose   # 同时透传设备日志
```

上传前先在本地校验镜像：首字节 `0xE9`、芯片标识 `0x0009`（ESP32-S3）、偏移 `0x20` 的应用
描述符（项目名必须是 `remapad_firmware`、版本取自构建时的 `git describe`）与 4 MB 分区上限。
上传按 16 帧一个窗口推送，收到设备 ACK（含期望序号与已收字节）才发下一窗；ACK 的期望序号
就是重发起点，因此超时重发不会重复写 flash。窗口末帧在帧头 `slot` 上带标记（末尾不足一窗也带），
设备收到即应答，不必等固定帧数；整窗重发时设备的序号错误应答按 50 ms 最小间隔限流。
升级在同一个会话里跑：桥接转发照常进行，命令与截图也能用。升级完成后设备重启，会话随链路
消失而退出；`--wait` 会重新打开端口并打印新版本。

设备侧行为与恢复路径见 [GETTING-STARTED.md](../docs/GETTING-STARTED.md) 的「固件 OTA 升级」。
设备仍在验证上一个镜像（开机 30 秒内的健康门槛）时会回 BUSY，等一会重试即可。

## 已知限制

- 家族表里的偏移多数取自公开资料，尚未逐条实机核对（只有 DualSense 蓝牙的 0x31
  行按 DualSense Edge 实测核对过）；核对前以 `--dump` 的结果为准，不符处回填
  `pad/layouts/` 下对应系列的文件。PS 系按 PID 分行（DS3、DS4、DualSense 有线都报 0x01），
  `--list` 的型号与连接方式可用于判断命中了哪一行。
- DualSense 蓝牙行的耳机状态字节（第 55 字节 / `PAD_HEADSET_PS`）已在 DualSense Edge 上核对，
  主机接受「插入」档（0x05 / 0x0D）而拒绝「带麦」档（0x07 / 0x0F，约 150 ms 后掉订阅），
  因此派生值只报插入；DS4 与 DS5 有线行未核对，方法见上文「3.5mm 耳机状态」。主机经 `0x002C`
  下发的耳机音频流当前只做日志留痕，尚未转发。
- 转发的是原始报告，不做任何按键重排：重排规则（用户自定义映射）在固件侧，本轮未做。
- 拔线或退出程序时发送 `DETACH` 帧，设备侧状态回到静置，不会留下卡住的按键。
- 反馈写回依赖固件里的输出报告描述（DS4 / DualSense / Xbox / DS3 / NS1 各一行），这些描述多数取自公开资料：
  DualSense 蓝牙（0x31）行已按 DualSense Edge 实机核对——帧头 `00 10` 加尾部 CRC32，缺 CRC 时手柄整份报告都不接受（写回却照样返回成功）；
  蓝牙上主机自己的连接动画会一直盖着灯，灯条设置（`valid_flag2` + `lightbar_setup`）必须与颜色写在同一帧里才压得住，玩家灯按五颗灯的模式表点亮。
  其余行同样未核对；写回没效果时先看该系列布局行的 `out` 描述。
- 手柄同时按住 L1+R1+L3+R3（约 300 ms）会被设备捕获成屏幕操控模式：设备先补一帧全松开、其后续发中性帧（玩家的按键不再上行），之后方向键移动屏幕焦点、圆圈键等价于点按屏幕。
  再按一次同样的组合退出（[ADR 0028](../docs/adr/0028-pad-combo-captures-screen.md)）。桥接程序不感知这个状态，转发照旧；
  不想要这个行为就别按这个组合，串口 `ui on` / `ui off` 可以直接置位验证。
