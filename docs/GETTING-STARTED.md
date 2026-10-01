# Remapad 新手开发与上手指南

本指南说明 Remapad 开发环境搭建、编译烧录、本地测试与调试方法。

## 前置环境

| 工具 | 版本/要求 | 用途 |
| :--- | :--- | :--- |
| Rust | 宿主 stable + Xtensa 工具链 (`esp`) | UI 界面编译与测试 |
| Python | 3.10+ (uv 托管) | 脚本与 PC 端工具 |
| ESP-IDF | `>=6.0,<6.2` | 固件核心编译与烧录 |
| 目标硬件 | ESP32-S3-Touch-LCD-1.69 | 16 MB Flash, 8 MB PSRAM, 240 × 280 ST7789V2 |

## 最短步骤

### 1. 准备 Rust 工具链

```powershell
uv run python scripts/setup-rust-toolchain.py          # 装 Xtensa 工具链（缺什么装什么）
uv run python scripts/setup-rust-toolchain.py --check  # 只检查，不改动环境
```

### 2. 预览界面与跑宿主用例

```powershell
cd ui/preview ; node tools/build.mjs                     # 首次构建 WASM 预览产物（自动补 wasm32 target 与 wasm-bindgen-cli）
cd ui/preview ; pnpm dev                                 # 浏览器预览同一套预览核心，存盘自动重编并刷新
cargo test --locked --manifest-path ui/Cargo.toml        # 界面宿主用例（元素几何 + 画面像素）
```

### 3. 检查界面改动

验证界面语法：`cargo check --manifest-path ui/Cargo.toml -p remapad-ui-wasm`（build.rs 会把界面源码全量编译一遍）。

### 4. 编译 ESP-IDF 固件

```powershell
cd firmware
idf.py set-target esp32s3
idf.py build
```
- 纯 C 无 UI 构建：`idf.py -DREMAPAD_UI=OFF build`。
- 发布构建：设置 `$env:REMAPAD_RELEASE = "1"` 后重编。

### 5. 烧录与监视

```powershell
idf.py -p COMx flash monitor
```

- 退出监视器快捷键：`Ctrl + ]`。

- 日常增量烧录应用分区：`idf.py -p COMx app-flash monitor`。

## 自动化测试

自动化测试命令：

```powershell
cargo test --locked --manifest-path ui/Cargo.toml   # 屏幕 UI：编译真实 .slint 产物，按元素几何与像素断言
cd ui/preview ; pnpm test                           # 屏幕 UI 浏览器端到端：WASM 预览 + Playwright 驱动 Chromium
uv run python scripts/firmware-test.py              # 固件主机端：把纯逻辑模块编译成本机可执行文件并运行
uv run python -m unittest discover -s pc/tests -t pc   # PC 侧：串口枚举、镜像校验、帧编解码与输出分流
```

## 代码风格与静态检查

```powershell
cargo fmt --manifest-path ui/Cargo.toml --all      # Rust 格式化（对 ui 工作区五个包生效）
cargo clippy --manifest-path ui/Cargo.toml         # Rust lint（CI 提级用 cargo clippy -- -D warnings）
clang-format -i <改动的 .c/.h>                      # C 格式化（不要对 firmware/components 运行）
```

## 串口 CLI 与 PWR 按键
通过 USB-Serial/JTAG 串口可执行命令控制与调试：

```powershell
uv run python pc/ctrl.py -p COM3 status          # 配对/角色/背光/息屏/运行时长/电池/版本/升级状态
uv run python pc/ctrl.py -p COM3 key circle      # 注入 circle（○）键（键名即内部值）
uv run python pc/ctrl.py -p COM3 key l1 800      # 注入 L1 键并保持 800 ms
uv run python pc/ctrl.py -p COM3 key release     # 立即释放注入的按键
uv run python pc/ctrl.py -p COM3 ui on           # 手动进出屏幕操控模式（on / off，不带参数看状态）
uv run python pc/ctrl.py -p COM3 stick l 4095 2048   # 左摇杆推满右（0-4095 或 center）
uv run python pc/ctrl.py -p COM3 stick reset     # 两侧摇杆回中
uv run python pc/ctrl.py -p COM3 link            # 链路快照：广播地址、连接间隔（itvl，4 = 5ms）、特性启用（feat）与上报计数
uv run python pc/ctrl.py -p COM3 headset auto    # 耳机状态字节：auto 按输入设备派生，也可钉住 0xNN 做主机侧 A/B
uv run python pc/ctrl.py -p COM3 shot            # 请求一次实机截图（PC 侧拼齐后存 PNG）
uv run python pc/ctrl.py -p COM3 trace 40        # 逐帧打印渲染耗时、提交耗时、damage 像素数与矩形条数（不带参数 60 帧）
uv run python pc/ctrl.py -p COM3 fwver 2.0.0     # 改写上报给主机的手柄固件版本（0x10 查询与出厂块共用；不带参数看当前值）
uv run python pc/ctrl.py -p COM3 version         # 运行镜像版本与分区、是否待验证
uv run python pc/ctrl.py -p COM3 rollback        # 回滚到上一个可用镜像（仅待验证状态）
uv run python pc/ctrl.py -p COM3 backlight 60    # 背光并持久化
uv run python pc/ctrl.py -p COM3 screen off      # 息屏（on 恢复）
uv run python pc/ctrl.py -p COM3 mode host       # 切到 host：COM 口消失，日志与 CLI 改走 UART0
uv run python pc/ctrl.py -p COM3 pad             # 识别到的手柄：来源、家族、型号、命中布局行、兜底与透传状态
uv run python pc/ctrl.py -p COM3 usb             # USB host 状态：角色、设备、收报告与写回计数、日志出口
uv run python pc/ctrl.py -p COM3 relay 0         # 关掉同代透传（默认开），观察解析重编码路径
uv run python pc/ctrl.py -p COM3 connect         # 连接键：开连接窗口等主机连上来（未配对身份进配对流程）
uv run python pc/ctrl.py -p COM3 pairing start   # 配新主机：断开当前主机后进发现广播，等新主机搜索配对（stop 停止广播并断链）
uv run python pc/ctrl.py -p COM3 wake            # 开唤醒窗口：未连接时发 0x81 把休眠主机叫起来，已连接则断开让它重连
uv run python pc/ctrl.py -p COM3 adv auto        # 广播窗口内的形态（auto 默认按窗口来源 / wake / reconnect），实机 A/B 对账用
uv run python pc/ctrl.py -p COM3 ctrl            # 手柄配色（ctrl [body button accent grip]，四段 0xRRGGBB，持久化；无参回读）
uv run python pc/ctrl.py -p COM3 advaddr         # 广播地址形态（auto / public / random，不落盘），分辨主机是否按地址形态过滤
uv run python pc/ctrl.py -p COM3 advpdu          # 广播 PDU 形态（auto / legacy / extended，不落盘）
uv run python pc/ctrl.py -p COM3 --amiibo Alm.bin # 上传 amiibo 镜像到设备 storage 分区槽位（540 或 572 字节 dump；amiibo list / select 0 选用）
uv run python pc/ctrl.py -p COM3 poweroff        # 关机（释放电源锁存，仅电池供电有效）
uv run python pc/ctrl.py -p COM3 reboot          # 软重启回 COM 模式
uv run python pc/ctrl.py -p COM3 --log --seconds 20         # 只读设备日志 20 秒
uv run python pc/ctrl.py -p COM3 --log --reset --seconds 25  # 先复位再抓完整启动日志
uv run python pc/ctrl.py -p COM3 --shot --out pc\shots\ui.png   # 抓实机截图并指定输出路径
uv run python pc/ctrl.py -p COM3 capture on        # 主机输出原始采集开（off 关）：震动/玩家灯/指令等主机输出经 0x12 帧回传
uv run python pc/ctrl.py -p COM3 --capture host-raw.log --seconds 30 --pad   # 抓 30 秒主机原始输出到文件（布局转换前），手柄转发照常
```

- **PWR 按键**：短按控制息屏与亮屏；长按 3 秒触发连接键（开启广播/断开连接）。
- **手柄操控屏幕**：`key ui` 注入组合键，进入后方向键移动焦点，`key a` 确认。
- **交互控制台**：不带命令运行 `ctrl.py` 进入交互模式，支持查看日志与直接发送 CLI 指令。

## 固件 OTA 升级

整包应用镜像（界面已编在应用里）可以在不接线烧录的情况下升级：PC 端把镜像经 USB-Serial/JTAG 推给设备，设备写进当前未运行的应用分区，`esp_ota_end` 校验通过后切换启动分区并重启。
设备侧实现在 `firmware/main/ota/`，PC 端入口是 [pc/ctrl.py](../pc/ctrl.py) 的 `--upgrade`：

```powershell
uv run python pc/ctrl.py --dry-run                  # 只校验镜像，不接设备
uv run python pc/ctrl.py -p COM3 --upgrade          # 升级默认镜像 firmware/build/remapad_firmware.bin（按脚本位置解析）
uv run python pc/ctrl.py -p COM3 --upgrade --image D:\build\remapad_firmware.bin
uv run python pc/ctrl.py -p COM3 --upgrade --wait   # 升级后等设备重启回来并打印新版本
uv run python pc/ctrl.py -p COM3 --upgrade --verbose  # 同时透传设备日志
```

### USB 手柄直插（host 模式）

- **切换与连接**：通过 UI「USB 模式」或串口 `mode host` 切换；直插手柄需向板卡「5V」脚（TP1）外灌 5V 供电，切回串口使用 `mode device`。
- **调试日志**：host 模式下串口改由 UART0 输出（GPIO43 TX, GPIO44 RX，115200 8N1）；亦可配置 WiFi 凭据通过 UDP（端口 9999）收听 netlog 日志。

## 常见问题

### WASM 预览页 404 或一直空白

先在 `ui/preview` 执行 `node tools/build.mjs` 生成 `dist/` 产物（首次要等几分钟），再起 `pnpm dev`；端口被占用时改用 `node tools/serve.mjs --port <端口>`。

### 屏幕上中文显示为方框（tofu）

动态拼接文本需将所用码点写入 `ui/src/app.slint` 的锚点字符串中，以便构建期烘焙字形位图。

### `cargo +esp 不可用` 或 `工具链缺 rust-src 组件`

运行 `uv run python scripts/setup-rust-toolchain.py` 安装 Xtensa Rust 工具链。

```powershell
uv run python scripts/setup-rust-toolchain.py               # 装 Xtensa 工具链
rustup component add rust-src --toolchain esp               # 缺 rust-src 时
```

### `Could not open COM3, the port is busy`

多半是上一轮 `idf.py monitor` 的 python 进程没退干净，占着串口。按进程精确清理后再烧录：

```powershell
Get-CimInstance Win32_Process |
  Where-Object { $_.CommandLine -like '*idf_monitor*' } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
```
