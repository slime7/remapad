# Remapad PC 侧工具（remapadctl）

`remapadctl.py` 是 PC 侧的命令行入口：一个进程同时做四件事——把手柄原始报告转发给设备、
当串口命令行、抓实机截图、推固件 OTA。设备只有一根 Type-C，USB-Serial/JTAG 既跑
桥接帧也跑固件日志与 CLI 文本，因此这些能力共用同一个串口句柄，彼此不再抢口。

```mermaid
flowchart LR
    Pad["手柄 HID 报告（hidapi）"] --> Session["Session：桥接帧编解码 + 串口唯一写者"]
    Session -->|"REPORT 帧"| Dev["设备"]
    Dev -->|"OUT_REPORT / FEEDBACK / HOST_RAW / 图像帧"| Session
    CLI["命令行与交互命令"] --> Session
    Shot["实机截图（--shot）"] --> Session
    OTA["固件 OTA（--upgrade）"] --> Session
    GUI["remapadgui.py（CustomTkinter）"] --> Session
    Session -->|"CLI 文本行"| Dev
```

整条会话只有一个串口写者：后台线程（音频触觉发送、抓包落盘）只置标志，帧与命令都由主循环写。

核心模块：

- `link.py`：桥接帧编解码、会话线程与免复位的 Win32 串口打开，是 PC 侧唯一的串口实现（含串口枚举与打开失败的提示文案）。
- `remapadctl.py`：桥接转发（读手柄、发桥接帧、把主机的震动与玩家灯写回手柄）、
  串口命令行、实机截图、固件 OTA 与交互式工具命令。
- `remapadgui.py`：上面这套会话的图形入口（CustomTkinter），与命令行共用同一份
  `Session` 与串口实现，只是把输出接到日志区、把控制做成按钮与输入框。

## 依赖

```powershell
cd pc
uv sync
```

依赖由 [uv](https://docs.astral.sh/uv/) 管理：版本要求写在 `pyproject.toml`，锁文件是
`uv.lock`，环境建在 `pc/.venv`。第三方依赖是 `hidapi`（读手柄）与 `customtkinter`
（图形界面，连带 darkdetect 与 packaging），Python 需要 3.10 或更高；uv 找不到合适的
解释器时会自己下载一个。串口与端口枚举直接走 Win32 API 与注册表，不依赖 pyserial。
当前实现只支持 Windows。

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
uv run python remapadctl.py -p COM3 --capture host-raw.log --seconds 30
                                          # 抓 30 秒主机原始输出（布局转换前）后退出
uv run python remapadctl.py -p COM3 --upgrade --wait
uv run python remapadctl.py -p COM3 --amiibo Alm.bin   # 上传 amiibo 镜像后退出
uv run python remapadctl.py -p COM3 --vid 0x054C --pid 0x0CE6 --max-rate 250 --no-rumble
uv run python remapadctl.py -p COM3 --logs            # 桥接的同时打印设备日志
```

`--all` 把设备的全部观测命令各发一遍（status / mem / version / link / pad / usb /
report / ui / adv / headset / fwver / fwack / fwpost / fwapply / ctrl / backlight /
screen / relay / motion / ltk / rumble / lamp / haptic），数据全部由固件现场读取——
不经过 UI 层，UI 冻结（截图期间、页面门控不取数）不影响实时性。

## 图形界面（remapadgui.py）

`remapadgui.py` 是同一套会话的图形入口，适合长时间挂着看日志、按固定动作做验收：

```powershell
cd pc
uv run python remapadgui.py
```

- 顶部工具条：选串口（下拉列出注册表里的 COM 口，默认落在本机第一个口上，只选中不自动连接；
  自己选过或敲过之后刷新不再改动选择）+ 连接 / 断开 + 状态灯 + 当前手柄摘要
  （长名字按词截断，完整描述在「会话」页的下拉里）。
- 「会话」页：转发开关（连接后默认开）、候选手柄下拉（与 `--list` 同一份枚举，选中哪只就只转发哪只）、
  转发计数、「列出候选接口」按钮（等价于 `--list`），以及配新主机 / 停止广播 / 唤醒主机 / 断开主机 /
  实机截图这些链路动作按钮。
- 「设置」页：把设备屏幕上的可改项搬到 PC，没有屏幕的设备也能改——亮度滑条与息屏开关
  （`backlight` / `screen`）、手柄配色四款预设与四段自定义 `0xRRGGBB`（`ctrl`）、
  DS4/DS5 的触摸板加减与截图键（`ds touchpad|capture`）、重启与关机（`reboot` / `poweroff`），
  外加一行设备信息（版本、分区、镜像、升级状态、电量、堆内存、运行时长、配对与角色）。
  控件值全部来自固件回读行：连接后自动读一次（status / version / ctrl / ds），
  「读取当前设置」按钮可以随时重读；界面不自己记状态。
- 「命令」页：调试口。输入框回车发送命令（↑ / ↓ 取历史），下面的按钮按输入注入 / 屏幕与连接 /
  诊断与状态分组，只把命令填进输入框、回车才发。连接键、屏幕操控、状态回读这些调试动作都在这里，
  界面上不给它们单独开按钮。
- 「升级」页：镜像路径与浏览、本地校验（同 `--dry-run`）、开始升级、进度条，以及
  「升级完成后等设备回来并重新连接」（等价于 `--upgrade --wait`）。
- 日志区：设备输出与工具提示逐行滚动，错误标红、发出去的整行命令带 `>` 前缀；
  可开关时间戳与自动滚动，可清空、导出成文本；输入框回车发送命令，↑ / ↓ 取历史，
  上方按钮把常用命令填进输入框。
- 底部状态栏：链路状态、转发计数与最近一次错误。
- 窗口默认 1040 × 860、最小 900 × 640：设置页与命令页放不下时自己滚（页签区和日志区各有
  最小高度，滚轮落在哪块就滚哪块），不必为了看全内容把窗口拉大。

界面与命令行共用 `Session`、`link.py` 与同一份命令处理，因此「串口只有一个持有者」的约束不变：
**界面与命令行不要同时连同一个口**。界面不做自动连接、不写配置文件；
截图在落盘 PNG 后用系统看图器打开（界面里不放图像预览，因此不需要 Pillow）。
关闭窗口会同步收尾当前会话：向设备补发 `DETACH` 并放掉串口，随后可以立刻改用命令行或重开界面连接。

## 串口命令面（完整控制手柄）

交互模式里不是 `:` 开头的行按固件 CLI 原样发送，手柄功能的完整控制面都在固件 CLI 里
（设备侧敲 `help` 有全表）：

```text
key a 200        注入按键（a b x y plus minus home capture c l r zl zr ls rs
                 up down left right gl gr ui），key release 全部松开
stick l 2048 2048  设摇杆电平 0-4095（stick reset 回中）
ctrl             手柄配色（ctrl 0x232323 0xa0a0a0 0xe6e6e6 0x323232，四段依次是
                 机身 按键 高光 握把，0xRRGGBB，持久化；无参回读）
connect          连接键：开连接窗口等主机连上来（未配对身份进配对流程）
pairing start    配新主机：断链 + 发现广播；pairing stop 停止广播并断链
wake             打开唤醒窗口；adv auto|wake|reconnect 钉窗口内形态
drop             断开当前主机
motion 3         0x09 运动块内容（0 全零 / 1 抓包占位 / 2 不带 / 3 真实样本）
headset 0x05     耳机状态字节（auto 回到按输入设备派生）
fwver 9.9.9      上报给主机的手柄固件版本；fwpost / fwack / fwapply 配套假升级
ltk 1            LTK 存储形态；relay 1 同代透传开关
rumble 200 0     手动震动（0-255 双侧，rumble off 停）；lamp 0xF 玩家灯；
                 haptic 0x10 触觉采样（输入设备有线接入时由板载蜂鸣器发声）——
                 与主机反馈走同一条编码投递路径
ui on            手柄操控屏幕模式（ui off 退出）
backlight 60     背光（持久化）；screen off 息屏；beep 蜂鸣
mode host        端口交给手柄（COM 口消失，日志与 CLI 改走 UART0，排查通道见
                 [../docs/GETTING-STARTED.md](../docs/GETTING-STARTED.md) 的「USB 手柄直插（host 模式）」）；
                 mode device 切回串口（屏幕会问是否立刻重启，复位是保底恢复路径）
ds touchpad on   DS4/DS5 手柄行为：触摸板映射加减键（左半减号 / 右半加号）；
                 ds capture off 让触摸板按下改发减号（默认开：发截图），两项都持久化
capture on       主机输出原始采集（off 关；开启后主机写进输出特征值的原始字节
                 经 0x12 帧回传 PC，PC 侧用 --capture / :capture 接住落盘）
amiibo list      amiibo 槽位列表（名称 + UID，当前选中带 *）；amiibo select 0 选用、
                 select off 取消、del 0 删除、poll on|off 手动开关射频场（无主机验证）
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
:capture [路径|off] 抓主机原始输出到文件（震动/玩家灯/指令，布局转换前；off 停止）
:ota [镜像路径]    推固件镜像（默认 ../firmware/build/remapad_firmware.bin）
:amiibo <bin 路径> 上传 amiibo 镜像到设备（540 纯镜像或 572 = 镜像 + 厂商签名，槽位名取文件名主干）
:quit              退出
```

`:shot` 与 `:ota` 的路径参数整段生效：路径里有空格也不用加引号（界面里的截图与升级按钮走同一条路径）。

手柄转发默认只在交互模式里开：一次性命令、`--shot`、`--log`、`--upgrade` 与 `--amiibo` 都不碰手柄（否则
主机会看到手柄闪一下），要在这些模式里也转发就加 `--pad`；`--no-pad` 在任何模式下都关掉转发。

虚拟手柄（Moonlight/Sunshine 串流时经 ViGEmBus 虚拟出的 DS4 等）会被 hidapi 枚举成普通
USB 手柄，按顺序选柄可能把它当桥接目标抓走——输入转发与震动写回全进虚拟设备，真手柄反而
时有时无。候选手单按设备树排除：祖先设备 ID 以 `VIGEM` 开头的接口不参与转发，`--list` 里以
「虚拟手柄，不参与转发」标注展示；设备树查不到的路径一律按真实手柄处理。

`--dump` 打印的每行是「时间戳 + 报告长度 + 原始字节」，用来与固件 `pad/layouts/` 里
对应系列的偏移对账：按住某个键只看一位变化，就能确认该键的字节与位序；摇杆推到极限
看量程与方向。对出来的偏移回填该系列的布局文件，比对结果与核对状态见
[../docs/controller-ps.md](../docs/controller-ps.md)。

## 会话与设备共用一根 Type-C

设备只有一根 Type-C：USB-Serial/JTAG 既跑固件日志与串口 CLI，也跑桥接帧。固件侧
`firmware/main/input/input_link.c` 是这条链路的唯一读取者，按帧头（`A5 5A`）把字节流
分成两类——桥接帧交给输入源，其余原样交给 CLI 行解析。PC 侧同样只有一个持有者：
`remapadctl.py` 一个进程同时收发，因此桥接运行期间命令、截图与升级都照常可用。

打开这个口绝不能让设备复位：片内状态机把 DTR/RTS 当复位控制线解释（RTS 拉高即复位，
两条同时拉高会让设备停在不运行应用的状态）。`link.py` 用 Win32 API 打开端口并在打开
前后把两条线固定为低电平；自己写 PC 端工具时按同样规则处理。

## 实机截图（--shot / :shot）

固件侧的串口 `shot` 命令只置一个标志：UI 任务在下一帧把当前画面按整屏
重渲染一遍（与面板上看到的是同一条渲染路径，含本机渲染加速器），再按 200 字节分块经
图像帧回传。PC 侧按偏移把分块拼齐，写进 `pc/shots/remapad-<时间戳>.png`（`--out` 可
指定路径）；缺块、越界或超时都不写文件，只报一行原因。
图形界面里的「实机截图」按钮走同一条命令，落盘后交给系统看图器打开。

截图期间屏幕会整屏刷一次、UI 冻结约 0.2–1 秒：这是调试通路，BLE 输入在另一个任务上，
不受影响。

## 3.5mm 耳机状态

固件把输入设备报告里的耳机状态经 NS2 报文报给主机（`0x09` 偏移 `0x0D` 与 `0x05` 的耳机
插入位）：输入设备侧的字节偏移与核对状态见 [../docs/controller-ps.md](../docs/controller-ps.md)
的「耳机状态」，主机侧接受的档位与取值对照见
[../docs/controller-switch2.md](../docs/controller-switch2.md) 的输入报告一节。
`headset <值>` 覆盖派生值、`headset auto` 回到按输入设备派生；要复现主机侧的取舍：
`headset 0x07`（或 `0x0F`）→ `wake` → `link` 看 `notify=--`。

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
`HOST_RAW`（0x12，设备 → PC，主机输出原始采集）、`FEEDBACK`（0x20，设备 → PC）、
截图帧 `IMAGE_INFO` / `IMAGE_DATA` / `IMAGE_END`（0x21-0x23，设备 → PC）、
OTA 升级用的 `OTA_BEGIN`（0x30）、`OTA_DATA`（0x31，载荷到 202 字节）、`OTA_END`（0x32）、
设备回发的 `OTA_ACK`（0x33）、amiibo 上传用的 `AMIIBO_BEGIN`（0x40）、`AMIIBO_DATA`（0x41）、
`AMIIBO_END`（0x42）与设备回发的 `AMIIBO_ACK`（0x43），以及 `PING`（0x7F）。

截图三帧的载荷：`IMAGE_INFO` 是宽 u16 小端 + 高 u16 小端 + 格式（1 = RGB565 小端）共 5 字节；
`IMAGE_DATA` 是整幅画面的字节偏移 u32 小端 + 最多 200 字节像素；`IMAGE_END` 是总字节数 u32 小端。
完整性由偏移覆盖满整幅画面判定，不再另做校验和。

amiibo 上传四帧（`--amiibo` 与交互模式 `:amiibo` 走这套）：
`AMIIBO_BEGIN` 是名称长度 u8 + 名称（UTF-8，1-31 字节）+ 镜像字节数 u32 小端（必须是 540，或 572 = 镜像 + 尾部 32 字节厂商签名）；
`AMIIBO_DATA` 是偏移 u16 小端 + 最多 200 字节数据（偏移越过已收字节数报错，重复帧幂等）；
`AMIIBO_END` 无载荷；设备对每帧回 `AMIIBO_ACK`：状态 + 错误码 + 已收字节 u32 小端 + 槽位号
（仅 DONE 有意义，0xFF 表示无）。收齐后设备落 storage 分区槽位（572 字节记录，纯镜像签名补零）并回 DONE，
串口 `amiibo select <n>` 选用（NFC 标签模拟见 [../docs/controller-switch2.md](../docs/controller-switch2.md) 的 NFC 章节）。

设备在主机下发 NS2 反馈（震动 / 玩家灯 / 触觉采样）时回发两种帧：
`FEEDBACK` 是归一化状态（打印与对账用，DS5 桥接时还驱动 PC 侧音频触觉合成；
其中触觉采样字节带原始采样 ID、仅打印展示——采样是主机点播的声音，固件把它按音色表
折成「强震 / 发声」铺色随 HD 子帧下发），
`OUT_REPORT` 是已经编码好的手柄输出报告——
震动与玩家灯的字段布局只在固件里有一份（`firmware/main/pad/feedback.c` 按设备布局行编码）。
PC 侧只把它交给 `hid.write()`，不参与任何映射（PS 系蓝牙形态的帧头与尾部 CRC32 也由固件算好）。
输出报告的字节数按设备布局行的 `out` 描述来，最长的两行是 DualSense 与 DualShock 4 的蓝牙形态
（78 字节），因此这一帧按 78 字节校验。默认开启，`--no-rumble` 关掉。
`FEEDBACK` 载荷 16 字节起（左右使能、两带强度、玩家灯、采样、两带频率落地值）；
设备声明了 HD 触觉时扩到 57 字节，附上固件按布局行重整出的时序子帧表（每侧有效子帧数 + 三个子帧的低/高频频率与增益 + 扬声器音色，
映射规则只在固件布局里有一份，PC 只做哑渲染）。
写回经 `WriteBackGate` 限速（30ms 一条、被挡的帧留最新一帧到期补写）：
游戏内震动包络逐包都变、设备侧去重压不住写回量，蓝牙 HID 写回又慢——
不加限速会把会话循环拖到输入转发卡顿。

`FEEDBACK` 帧的打印按秒合并（`FeedbackThrottle`）：震动效果的包络逐帧在变，
逐条打印会把日志区刷爆；窗口内只打第一条，
下一条带「已合并 N 条」。写回手柄与帧计数不受限频影响。

## 主机原始输出采集（--capture / :capture）

这是看「主机到底发了什么」的抓包通路：固件把主机写进输出特征值的**原始字节**
（震动参数包、指令帧、复合输出、固件更新记录流与扩展通道）在解析成结构化事件、
按布局编码之前的形态直接回传 PC。与 `OUT_REPORT` / `FEEDBACK` 的区别：
那两帧是固件消化过的结果（归一化状态 / 按输入设备布局重新编码的报告），
`HOST_RAW` 是主机原文，供协议对账与问题定位。

```powershell
uv run python remapadctl.py -p COM3 --capture host-raw.log --seconds 30 --pad
                                          # 抓 30 秒，手柄转发照常（实体手柄连着串口也能抓）
```

交互模式里 `:capture <路径>` 开始、`:capture off` 停止、`:capture` 看状态；
`--capture` 是同一能力的一次性形态（`--seconds` 控制时长，0 = 到 Ctrl+C）。
落盘是文本格式，一行一条记录：

```text
+0.500s rumble[0x12] seq=000  32B 7c 04 80 01 97 63 ...
+1.250s cmd[0x14] seq=001   9B 09 91 01 07 00 01 00 00 01
```

通道字节取 controller-switch2.md「GATT 属性表」的句柄低字节（`base-config` 0x05、
`rumble` 0x12、`cmd` 0x14、`composite` 0x16、`fwupg` 0x18、扩展通道 0x22-0x32，
含音频下行 0x2C）；单条写入超过 253 字节（升级数据块）截断并带 `trunc` 标记；
`seq` 是设备侧记录号，跳号说明设备队列满、丢过包（收尾总结里按处数汇总）。
采集默认关闭（震动流约 66 Hz，开着会持续占用串口），设备侧开关是串口命令
`capture on|off`，`:capture` / `--capture` 会自己发送；会话结束或 `:capture off`
时自动发 `capture off` 并落总结一行。

## DS5 音频触觉（桥接路径，--no-audio-haptics 关闭）

DualSense 连在 PC 上时音频接口由 PC 持有，触觉与喇叭改由 PC 侧送，按连接方式走两条通路：

- **USB 直插 PC**：对 4ch 扬声器端点开 WASAPI 共享流，通道 3/4（音圈）放触觉子帧、
  1/2（手柄小喇叭）放发声段，没有声音时两路静音。
- **蓝牙连接 PC**（默认启用，`--no-bt-haptics` 关掉回落 0x31 两带震动）：有 PyAV/libopus 时用 0x36
  （触觉 PCM + Opus 喇叭块，发声段由手柄真喇叭出声），没有 libopus 时回落 0x32（发声段折进两侧音圈）。

子帧参数吃 `FEEDBACK` 帧的 57 字节 HD 版（固件已按布局行 `hd` 规则重整好，PC 只做哑渲染；
老固件的 16 字节帧回落两带正弦、扬声器恒零）。两条通路都按内容门控（静默整流停发），
启用后发固件命令 `haptic audio on` 让 HID 震动字节让位，会话退出或断开时 `haptic audio off` 复位，
开流失败或写回被拒静默回落 HID。
报文布局、承载选择、让位语义、增益与核对状态见 [../docs/controller-ps.md](../docs/controller-ps.md)
的「音频触觉与 HD 触觉」，取舍见 [ADR 0042](../docs/adr/0042-ds5-audio-haptics-onboard-synthesis.md)、
[ADR 0043](../docs/adr/0043-ds5-bridge-pc-side-audio-haptics.md) 与
[ADR 0046](../docs/adr/0046-ns-waveform-to-ds5-pcm-hd-haptics.md)。

## 固件 OTA（--upgrade）

```powershell
uv run python remapadctl.py --dry-run                     # 只校验镜像，不接设备
uv run python remapadctl.py -p COM3 --upgrade             # 升级默认镜像 firmware/build/remapad_firmware.bin
uv run python remapadctl.py -p COM3 --upgrade --wait      # 等设备重启回来并打印版本
uv run python remapadctl.py -p COM3 --upgrade --verbose   # 同时透传设备日志
```

上传前先在本地校验镜像：首字节 `0xE9`、芯片标识 `0x0009`（ESP32-S3）、偏移 `0x20` 的应用
描述符（项目名必须是 `remapad_firmware`、版本取自构建时的 `git describe`）与 4 MB 分区上限。
图形界面「升级」页把同一套检查做成「校验镜像」按钮，推送与进度显示走同一条 `:ota` 命令；
勾选「升级完成后等设备回来并重新连接」等价于 `--upgrade --wait`。
上传按 16 帧一个窗口推送，收到设备 ACK（含期望序号与已收字节）才发下一窗；ACK 的期望序号
就是重发起点，因此超时重发不会重复写 flash。窗口末帧在帧头 `slot` 上带标记（末尾不足一窗也带），
设备收到即应答，不必等固定帧数；整窗重发时设备的序号错误应答按 50 ms 最小间隔限流。
升级在同一个会话里跑：桥接转发照常进行，命令与截图也能用。升级完成后设备重启，会话随链路
消失而退出；`--wait` 会重新打开端口并打印新版本。

设备侧行为与恢复路径见 [GETTING-STARTED.md](../docs/GETTING-STARTED.md) 的「固件 OTA 升级」。
设备仍在验证上一个镜像（开机 30 秒内的健康门槛）时会回 BUSY，等一会重试即可。

## 主机端用例

与设备无关的 PC 侧逻辑有一组标准库 `unittest` 用例（串口枚举、镜像校验、帧编解码、输出分流与工具命令解析），
不需要接设备：

```powershell
cd pc
uv run python -m unittest discover -s tests -t . -v
```

仓库根的写法与这里一致（`cd pc ; uv run python -m unittest discover -s tests -t .`）。用例跑的是 `pc/` 下的真源码，不复制被测逻辑；
范围与规则见 [TESTING.md](../docs/TESTING.md) 的「PC 侧主机端用例」。

## 已知限制

- 家族表里的偏移多数取自公开资料，尚未逐条核对；哪些行已核对、哪些字段还是初值以
  [../docs/controller-ps.md](../docs/controller-ps.md) 的「核对状态」为准，核对前先看 `--dump` 的结果，
  不符处回填 `pad/layouts/` 下对应系列的文件。PS 系按 PID 分行（DS3、DS4、DualSense 有线都报 0x01），
  `--list` 的型号与连接方式可用于判断命中了哪一行。
- 耳机状态字节与主机侧接受的档位见 [../docs/controller-ps.md](../docs/controller-ps.md) 与
  [../docs/controller-switch2.md](../docs/controller-switch2.md)，复核步骤见上文「3.5mm 耳机状态」。
  主机经 `0x002C` 下发的耳机音频流只做日志留痕：传输协议公开资料有限，无法落实转发。
- 转发的是原始报告，PC 侧不改写报告内容：按键含义由设备按输入设备的家族布局解释。
- 拔线或退出程序时发送 `DETACH` 帧，设备侧状态回到静置，不会留下卡住的按键。
- 反馈写回依赖固件里的输出报告描述（DS4 / DualSense / Xbox 蓝牙 / XInput / DS3 / NS1 各一行）：
  描述与核对状态见 [../docs/controller-ps.md](../docs/controller-ps.md) 与其余 `docs/controller-*.md`；
  写回没效果时先看该系列布局行的 `out` 描述。
- 手柄同时按住 L1+R1+L3+R3（约 300 ms）会被设备捕获成屏幕操控模式：设备先补一帧全松开、其后续发中性帧（玩家的按键不再上行），之后方向键移动屏幕焦点、圆圈键等价于点按屏幕。
  再按一次同样的组合退出（[ADR 0028](../docs/adr/0028-pad-combo-captures-screen.md)）。桥接程序不感知这个状态，转发照旧；
  不想要这个行为就别按这个组合，串口 `ui on` / `ui off` 可以直接置位验证。
