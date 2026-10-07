# Remapad PC 侧工具（ctrl）

`pc/src/ctrl.js` 负责将 PC 手柄输入转发给设备、提供串口 CLI、捕获截图与推送固件 OTA。

```mermaid
flowchart LR
    Pad["手柄 HID 报告（node-hid）"] --> Session["Session：桥接帧编解码 + 串口唯一写者"]
    Session -->|"REPORT 帧"| Dev["设备"]
    Dev -->|"OUT_REPORT / FEEDBACK / HOST_RAW / 图像帧"| Session
    CLI["命令行与交互命令"] --> Session
    Shot["实机截图（--shot）"] --> Session
    OTA["固件 OTA（--upgrade）"] --> Session
    GUI["gui-server（Node 后端 + 浏览器前端）"] --> Session
    Session -->|"CLI 文本行"| Dev
```

核心模块（Node 22 ESM，pnpm workspace 管理）：

- `src/link/`：桥接帧编解码、免复位 Win32 串口驱动（koffi 直调）与 netlog UDP 链路。
- `src/session/`：会话主循环、设备回读解析、截图、OTA 与 amiibo 上传。
- `src/input/`：手柄枚举与设备树归属推导；`src/haptics/`：DS5 音频触觉（Opus + WASAPI）。
- `src/ctrl.js`：输入转发、串口 CLI、截图、OTA 与工具命令。
- `src/gui-server/` + `gui/`：图形控制台（Node 后端 + Vue 3 / mde-vue 前端）。
- `src/mcp/`：按键注入 MCP 服务（stdio）。

## 依赖

```powershell
pnpm install
```

PC 工具依赖 Windows 环境与 Node 22、pnpm；原生依赖（koffi、node-hid、opus、audify）由 pnpm
安装时预编译，仓库根 `package.json` 的 `pnpm.onlyBuiltDependencies` 已放行其构建脚本。
图形界面前端另需构建一次（开发调试用 `pnpm --filter @remapad/gui dev`）：

```powershell
pnpm --filter @remapad/gui build
```

## 用法

以下命令在仓库根执行（也可以用 `pnpm --filter @remapad/pc ctrl -- <参数>` 走包脚本）：

```powershell
node pc/src/ctrl.js --list                    # 列出候选的手柄接口
node pc/src/ctrl.js --dump --seconds 10       # 采集 10 秒原始报告（核对布局用）
node pc/src/ctrl.js -p COM3                   # 桥接 + 交互命令行
node pc/src/ctrl.js -p COM3 --no-pad          # 只当串口命令行用，不转发手柄
node pc/src/ctrl.js -p COM3 status            # 执行一条设备命令后退出
node pc/src/ctrl.js -p COM3 --all             # 拉取设备全部观测数据后退出
node pc/src/ctrl.js -p COM3 --shot            # 实机截图存成 PNG
node pc/src/ctrl.js -p COM3 --log --seconds 20
node pc/src/ctrl.js -p COM3 --log --reset --seconds 25
node pc/src/ctrl.js -p COM3 --capture host-raw.log --seconds 30
                                         # 抓 30 秒主机原始输出（布局转换前）后退出
node pc/src/ctrl.js -p COM3 --upgrade --wait
node pc/src/ctrl.js -n 192.168.1.5 --upgrade
                                         # 走 WiFi 的 netlog 通道推 OTA（会话开着才行；截图不能走网络）
node pc/src/ctrl.js -p COM3 --amiibo Alm.bin   # 上传 amiibo 镜像后退出
node pc/src/ctrl.js -p COM3 --vid 0x054C --pid 0x0CE6 --max-rate 250 --no-rumble
node pc/src/ctrl.js -p COM3 --logs            # 桥接的同时打印设备日志
```

`--all` 依次拉取全部观测命令回读。

## 图形界面（gui-server + gui）

日常启动：

```powershell
node pc/gui/dev.mjs                           # 默认 http://127.0.0.1:8787/，--http-port 换端口
```

发布包用构建产物托管前端，见下文「发布包」。别用 pnpm 间接跑 vite，Ctrl+C 会留下孤儿进程占着端口。

## 发布包

`pnpm build` 产出免安装的发布包到 `pc/dist/`：内含 Node 运行时、pc 源码与生产依赖、
前端构建产物与 `remapad.cmd` 启动器，目标机器无需安装 Node：

```text
remapad.cmd gui      # 图形控制台，浏览器打开 http://127.0.0.1:8787/
remapad.cmd ctrl -p COM3
remapad.cmd mcp
```

包随构建机的平台与架构（当前 Windows x64）；仓库里另有 `node pc/gui/dev.mjs` 供开发热更新。

功能分区：
- **工具条**：串口/网络选择、连接控制与状态显示（同一串口不可与 CLI 同时打开）。
- **会话**：输入转发开关、手柄选择与链路控制按键。
- **设置**：屏幕亮度、手柄配色、DS4/DS5 按键映射与 WiFi 凭据配置。
- **命令 / 升级 / 日志**：串口 CLI 命令交互、OTA 升级推送与实时日志监控。

界面主题跟随系统，右上角按钮可在跟随系统 / 浅色 / 深色间循环并记住选择。

### 网络连接（WiFi UDP 调试通道）

通过设备 UDP 调试端口（默认 9999）进行网络通信。支持手柄转发、CLI 与 OTA，不支持截图。

## 按键注入 MCP 服务（src/mcp/）

把「PC → 设备 → NS2 主机」的按键注入包成 stdio MCP 服务供 agent 调用：

```powershell
node pc/src/mcp/cli.js                 # 启动后不碰设备，连接由 remapad_connect 完成
node pc/src/mcp/cli.js -p COM3         # -p / -n 只是 remapad_connect 无参时的默认目标
```

调用模型是显式连接：先 `remapad_connect`（串口或 WiFi 二选一）建链，之后按键工具才可用，
结束 `remapad_disconnect` 断链（key release 全松、释放串口）。

```mermaid
flowchart LR
    Agent["Agent（MCP 客户端）"] -->|"stdio"| Svc["src/mcp/cli.js"]
    Svc -->|"connect / disconnect"| Link["串口或 netlog UDP 链路"]
    Svc -->|"状态变更 / 续期"| Engine["按键状态引擎（自持按下表）"]
    Engine -->|"key / stick CLI 命令"| Session["ctrl.Session（串口唯一写者）"]
    Session -->|"调试注入"| DP["dp_source 叠加"]
    Session -->|"CLI 文本行"| Dev["设备"]
    Dev -->|"NS2 报告"| Host["NS2 主机"]
```

按键编排落在 PC 侧按键状态引擎（[ADR 0063](../docs/adr/0063-mcp-cli-key-injection-engine.md)）：
注入走固件调试 CLI，无周期心跳流量，与实体手柄转发可并存（按键按位叠加）。tap 在无按住键时直发一条 `key`（固件到点自动松开）；
按住键由引擎维护按下表并以滚动期限看门狗续期；固件没有单键松开，子集松开是
「key release 全松 + 重发存活键」，存活键在边界上有至多一个 dp 拍（5ms）的瞬断，
时值精度按 ±10ms 量级理解。UDP 通道空闲时服务发 PING 保活 2 秒桥接窗口。工具面：

| 工具 | 语义 |
| :--- | :--- |
| `remapad_connect` | 连接设备：`port`（串口号）或 `net`（IP[:端口]，WiFi netlog）二选一，无参用启动默认 |
| `remapad_disconnect` | 断开设备链路：key release 全松、回中并释放串口/网络 |
| `remapad_status` | 链路状态与引擎按键状态；已连接时附设备回执（status/pad/link 原始行） |
| `remapad_pair` / `remapad_drop` | 连接键动作（开连接窗口）/ 断开与主机的 BLE 连接 |
| `remapad_tap` | 单键/组合键，hold_ms 后松开（同起同落，阻塞到松开） |
| `remapad_hold` | 按住/松开不限时长，可长按期间做别的调用 |
| `remapad_stick` / `remapad_stick_reset` | 摇杆电平 0-4095（2048 中位，y 向上为正）/ 回中 |
| `remapad_script` | 时间线脚本：t + down/up/tap/stick 任选、loop 循环，结束自动全松回中 |
| `remapad_replay` | 回放按键记录文件（TAS 式逐帧）：后台执行、立即返回，期间按键工具被拒绝 |
| `remapad_replay_stop` | 打断进行中的回放（全松回中）；没有回放时幂等成功 |
| `remapad_release_all` | 全部松开并回中（逃生口；回放进行中被拒绝） |
| `remapad_screenshot` | 实机截图（仅串口会话） |

键名即固件调试注入的按键位名（`pad_state.h` 内部值，PS 位置语义）：
`triangle circle cross square l1 r1 l4 r4 l3 r3 up down left right opt touchpad home share mute`；
主机侧语义由固件布局映射（circle/cross/triangle/square→A/B/X/Y、opt→+、touchpad→-、
home→HOME、share→capture、mute→C、l4/r4→GL/GR 背键、l1/l3/r1/r3→L/LS/R/RS）。
模拟扳机（ZL/ZR 的模拟量）不在 CLI 注入面里，需要真手柄转发。
注意：服务独占链路，与 ctrl / gui 不能同开一个串口。

### 按键回放（remapad_replay）

把提前录制或程序生成的按键记录文件（TAS 式逐帧输入表，决策见 [ADR 0064](../docs/adr/0064-mcp-key-replay.md)）
交给服务后台回放：立即返回，同一时刻只有一条回放任务，进度随 `remapad_status` 常态回报。
回放进行中 `remapad_tap` / `remapad_hold` / `remapad_stick` / `remapad_stick_reset` /
`remapad_script` / `remapad_release_all` / `remapad_replay` 一律被拒绝，错误信息带当前帧进度，
防止误发按键或误打断；打断只走 `remapad_replay_stop`（`remapad_disconnect` 兜底打断），
结束、打断与循环边界都会全松按键并回中摇杆。规模上限：文件 4 MiB、展开事件 65536、
总时长 `--replay-max-ms`（默认 10 分钟）；时间刻度按帧长换算，时值精度与注入同为 ±10ms 量级。

记录是 UTF-8 文本（LF / CRLF 均可），`#` 开头是注释；头部 `key = value` 定时间刻度：
`frame_ms=每帧毫秒` 或 `fps=帧率`（缺省 15ms，即 NS2 上报节奏）；帧行一行一条：

```text
|帧号|按键+按键|左摇杆x,y|右摇杆x,y|
```

帧行表示「从该帧起的输入状态」并保持到下一帧行，帧号必须逐行递增、中间缺的帧就是保持拍；
按键字段 `.` 或空 = 无按键，多个键用 `+` 连接（键名同工具面）；摇杆 `x,y` 取 0-4095（2048 中位），
`.` 或空 = 保持上一拍。示例（60fps，开局按住 circle+cross 推左摇杆，半秒后全放，一秒处回中）：

```text
# 开场压制
fps = 60
|0|circle+cross|4095,2048|
|30||.|
|60|.|2048,2048|
```

独立的蓝牙直气回放样例见 `pc/test/samples/pad_replay.mjs`
（`node pc/test/samples/pad_replay.mjs <采集文件>`，私有触觉流与 HID 震动两种落点）。

## 串口命令面（完整控制手柄）

交互模式里不是 `:` 开头的行按固件 CLI 原样发送，手柄功能的完整控制面都在固件 CLI 里
（设备侧敲 `help` 有全表）：

```text
key circle 200   注入按键（键名即内部值：circle cross triangle square opt touchpad home share mute
                 l1 r1 l4 r4 l3 r3 up down left right ui），key release 全部松开
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
`--all` 拉的就是这批回读。`mem` 的应答在下一帧打出：PSRAM 与内部堆的空闲、最大块与
历史最低，观察内存趋势不用等 60 秒一条的周期日志。

交互模式里 `:` 开头的是本工具命令：

```text
:help              显示工具命令清单
:all               拉取设备全部观测数据（同 --all）
:shot [路径]       抓实机截图并存成 PNG
:log [秒|off]      透传设备日志（0 表示持续到 :log off）
:capture [路径|off] 抓主机原始输出到文件（震动/玩家灯/指令，布局转换前；off 停止）
:ota [镜像路径]    推固件镜像（默认 firmware/build/remapad_firmware.bin）
:amiibo <bin 路径> 上传 amiibo 镜像到设备（540 纯镜像或 572 = 镜像 + 厂商签名，槽位名取文件名主干）
:quit              退出
```

## 会话与设备共用一根 Type-C

USB-Serial/JTAG 既跑桥接帧也跑串口 CLI。打开端口时必须固定 DTR/RTS 为低电平，避免触发硬件复位。

## 实机截图（--shot / :shot）

截取当前设备屏幕内容并保存为 PNG 图片（默认路径 `pc/shots/`）。

## 3.5mm 耳机状态

支持通过 `headset <值>` 覆盖上报主机的耳机状态，或通过 `headset auto` 自动派生。

## 桥接帧格式

```text
A5 5A | ver | type | slot | seq | len | payload[len] | crc16(LE)
```

CRC-16/CCITT-FALSE 校验除末尾 2 字节外的整帧。常见帧类型：
- `ATTACH` (0x01) / `DETACH` (0x02)：设备插入/拔出事件。
- `REPORT` (0x10)：输入设备原始报告。
- `OUT_REPORT` (0x11) / `FEEDBACK` (0x20)：主机反馈手柄输出报告与归一化状态。
- `HOST_RAW` (0x12)：主机原始输出捕获。
- `IMAGE_INFO` / `IMAGE_DATA` / `IMAGE_END` (0x21-0x23)：截图数据分块。
- `OTA_BEGIN` / `OTA_DATA` / `OTA_END` / `OTA_ACK` (0x30-0x33)：固件 OTA 传输。
- `AMIIBO_BEGIN` / `AMIIBO_DATA` / `AMIIBO_END` / `AMIIBO_ACK` (0x40-0x43)：amiibo 镜像上传。

## 主机原始输出采集（--capture / :capture）

捕获主机向手柄下发的原始报文（震动、LED、指令等），文本格式记录：

```powershell
node pc/src/ctrl.js -p COM3 --capture host-raw.log --seconds 30 --pad
```

```text
+0.500s rumble[0x12] seq=000  32B 7c 04 80 01 97 63 ...
+1.250s cmd[0x14] seq=001   9B 09 91 01 07 00 01 00 00 01
```

## DS5 音频触觉（桥接路径，--no-audio-haptics 关闭）

- **USB 直插 PC**：WASAPI 4ch 音频流驱动触觉与喇叭（audify）。
- **蓝牙连接 PC**：优先 0x36（触觉 PCM + Opus 编码喇叭），缺失原生 Opus 时回落 0x32。

## 固件 OTA（--upgrade）

```powershell
node pc/src/ctrl.js --dry-run                     # 只校验镜像，不接设备
node pc/src/ctrl.js -p COM3 --upgrade             # 升级默认镜像 firmware/build/remapad_firmware.bin
node pc/src/ctrl.js -p COM3 --upgrade --wait      # 等设备重启回来并打印版本
node pc/src/ctrl.js -p COM3 --upgrade --verbose   # 同时透传设备日志
```

## 局域网日志收听

与固件 netlog 命令配套的 UDP 终端（收日志、发 CLI，命令见 `--help`）：

```powershell
node scripts/netlog_listen.mjs --port 9999
```

## 主机端用例

运行 PC 侧纯逻辑测试：

```powershell
pnpm test
```

静态检查：

```powershell
pnpm lint            # eslint
pnpm lint:fix        # eslint 自动修复
pnpm lint:style      # stylelint
```

## 已知限制

- 仅转发原始报告，不改写按键语义；退出时发送 `DETACH` 帧避免留存卡键。
- L1+R1+L3+R3 长按 300ms 进入屏幕操控模式，再次长按或长按 ✕ 键退出。
