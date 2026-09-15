# 架构决策记录 (ADR)

本目录记录 Remapad 项目中具有长期影响的重要架构决策、技术选型背景及其取舍权衡。

## 为什么需要 ADR

架构决策记录（Architecture Decision Record, ADR）捕获决策做出时的具体背景、考虑的候选方案及正面与负面影响，为后续维护团队和 coding agent 提供权威的决策依据，防止陷入重复推翻已有架构权衡的循环。

## 文件与状态规范

- **文件命名**：采用 `NNNN-slug.md` 格式，编号从 `0001` 开始单调递增，四位数字对齐。
- **状态流转**：
  - `proposed`：决策正在提议或评审中。
  - `active`：决策已被批准并作为当前生效的架构基准。
  - `superseded`：决策已被新的 ADR 完整取代（取代关系必须在新 ADR 的 `Supersedes`/`替代` 元数据和本索引中可追溯，不得回写旧正文）。
  - `retired`：决策所针对的功能或组件已被废弃淘汰。
- **决策边界**：一份 ADR 仅专注记录一项具体的长期决策。日常的缺陷修复、小范围样式微调或无重大架构影响的重构无需编写 ADR。
- **历史记录不可变**：已创建 ADR 的标题、日期、背景、决策、考虑的方案、影响和正文格式均属于历史记录，禁止为了适配当前实现、修正措辞或重新排版而修改。
- **取代关系必须追加记录**：发现新事实或需要改变架构基准时，必须创建新的 ADR，不得改写旧 ADR 的决策内容。新 ADR 必须在 `Supersedes`/`替代` 元数据中链接被取代的编号，并明确是完整取代还是仅取代其中的范围；部分取代时，旧 ADR 仍适用的范围必须保留为 `active`，并在本文件索引中说明。
- **旧 ADR 唯一允许的生命周期修改**：旧 ADR 只允许变更 `Status`/`状态` 元数据，以反映 `superseded` 或 `retired` 等状态；不得修改其他字段或正文。取代关系以新 ADR 和本索引为准，不得通过改写旧正文补充解释。
- **索引是强制清单**：新增、取代、部分取代或废弃任何 ADR 后，必须同步更新下方“当前决策”表。表格必须从 `0001` 开始按编号升序列出 `docs/adr/` 下全部 `NNNN-*.md` 文件，不得遗漏、合并或只保留最后一条；状态必须与对应 ADR 元数据一致。

## 当前决策

下表是本目录的完整 ADR 索引，也是当前决策状态的唯一导航入口。

| ADR | 状态 | 主题 |
| --- | --- | --- |
| [0001](0001-use-pocketjs-vue-vapor-for-esp32s3-ui.md) | active | 采用 PocketJS 与 Vue Vapor 驱动 ESP32-S3 屏幕 UI |
| [0002](0002-adopt-hardware-bridge-and-packaging-architecture.md) | active | 引入统一硬件桥接协议与双工作区分层架构；打包部分由 0003 取代 |
| [0003](0003-use-official-esp-idf-host.md) | active | 采用官方 PocketJS ESP-IDF host 构建链路 |
| [0004](0004-use-local-pocketjs-checkout.md) | active | PocketJS 组件与原生归档改由本地 checkout 提供，Web 预览切换为官方开发主机 |
| [0005](0005-vendor-pocketjs-idf-components.md) | active | ESP-IDF 组件与原生归档固定在本仓库，pocketjs 仅作开发参考 |
| [0006](0006-product-owner-task-for-pocketjs-guest.md) | active | 由产品 owner task 承载 PocketJS guest 生命周期 |
| [0007](0007-esp-lcd-panel-touch-bsp.md) | active | 显示与触摸 BSP 采用 esp_lcd 内置驱动与 Registry 触摸组件；strip 提交复用语义由 0008 部分取代 |
| [0008](0008-panel-transfer-completion-gate.md) | active | 面板提交增加传输完成门控，部分取代 0007 的颜色缓冲复用语义 |
| [0009](0009-ota-storage-flash-layout.md) | active | 固化 16MB Flash 分区终局布局：OTA 双分区与通用存储区 |
| [0010](0010-nimble-ble-controller-stack.md) | active | BLE 手柄外设采用 ESP-IDF 内置 NimBLE 栈，SMP 关闭、配对由应用层 Command 0x15 承担 |
| [0011](0011-controller-dataplane-module-boundary.md) | active | 控制器数据面按 ns2/ble/dp/usb 四模块分层，数据流单点汇合于 dp_task；目录划分由 0021 部分取代，数据面汇合约定仍生效 |
| [0012](0012-firmware-boot-splash-before-ui.md) | active | 固件在 PocketJS UI 就绪前自绘启动画面并提前点亮背光 |
| [0013](0013-defer-page-mount-after-first-frame.md) | superseded | 首帧只挂载首页，其余页面按帧补挂并在首页显示加载提示；由 0014 取代 |
| [0014](0014-page-mount-on-demand-progressive-fill.md) | superseded | 页面改为按需挂载并逐帧自顶向下填充，外层容器先出现；由 0015 取代 |
| [0015](0015-restore-deferred-page-mount-after-first-frame.md) | superseded | 恢复首帧后逐帧补挂页面，放弃按需挂载与分帧填充；由 0016 取代 |
| [0016](0016-mount-all-pages-before-first-frame.md) | active | 首屏前一次性挂载全部页面，放弃首帧后逐帧补挂 |
| [0017](0017-display-path-and-scroll-frame-budget.md) | active | 面板 SPI2 取 40 MHz、strip 改 32 行条带优先内部 RAM、本机加速回调接管填充与掩码混合；整幅内容按行带顺序在一帧内刷完（隔行因实机纵向错位被否决）；面板时钟取值由 0018 部分取代 |
| [0018](0018-panel-spi2-clock-80mhz.md) | active | 面板 SPI2 时钟取上限 80 MHz，部分取代 0017 决策 1 的时钟取值 |
| [0019](0019-playwright-e2e-and-host-unit-tests.md) | active | 以 Playwright 端到端测试与固件主机端单元测试作为回归基线 |
| [0020](0020-battery-adc-sampling-and-charge-inference.md) | active | 电池电量走 BAT_ADC 采样（过采样平均 + 曲线拟合校准 + 静置电压—容量表），充电状态按电压趋势推断 |
| [0021](0021-input-path-three-stage-layering.md) | active | 输入通路按 input/pad/target 三段分层（接收 / 处理 / 转换），部分取代 0011 的目录划分 |
| [0022](0022-ota-over-bridge-frames-with-rollback.md) | active | OTA 升级复用桥接帧（USB-Serial/JTAG 双分区回写）与回滚健康门槛 |
| [0023](0023-ns2-sub-spec-conn-interval-and-wake-burst.md) | active | NS2 手柄链路采用亚规范连接间隔（5 ms）与显式唤醒广播（0x81 突发）；广播形态与唤醒突发由 0024 部分取代，连接间隔、特性启用门槛与上报节奏仍生效 |
| [0024](0024-ns2-steady-wake-adv-and-pairing-key.md) | active | 已配对身份常驻唤醒广播（0x81 + 主机地址）自动回连，配对页按钮改真机配对键语义，切换手柄走断连重连；部分取代 0023 的广播形态与唤醒突发；广播里的主机地址来源由 0030 部分取代 |
| [0025](0025-pad-layout-modules-per-series.md) | active | 家族布局按系列分文件登记（layout.h/layout.c + layouts/），注册表统一匹配；补全 0021 在 pad/ 内的文件划分 |
| [0026](0026-same-generation-input-passthrough.md) | active | 手柄输入按同代透传、异代解析分发：设备自带报告语言与目标语言一致时原样转发报文体 |
| [0027](0027-runtime-usb-role-switch.md) | active | USB host 直插采用运行时角色切换（日志改走 UART0），复位回到串口 |
| [0028](0028-pad-combo-captures-screen.md) | active | 手柄组合键 L1+R1+L3+R3 捕获为屏幕操控（方向键移动焦点、圆圈键确认），捕获期间停发报文并补一帧全松开 |
| [0029](0029-pad-ui-axis-split.md) | active | 手柄操控屏幕的方向键分两个轴：上下走页面内容（到末尾再按下继续滚到页底）、左右只在底栏两项之间走；实机 L1 / R1 等价于左右 |
| [0030](0030-ns2-wake-adv-host-address.md) | active | NS2 唤醒与回连广播携带主机最近一次连接记录到的地址（凭证地址作兜底），部分取代 0024 的广播地址来源 |

## 创建 ADR 脚本用法

项目已内置 ADR 生成脚本 [scripts/create_adr.py](../../scripts/create_adr.py)，基于 Python 3.10+ 运行。不要手动编写或硬编码文件编号，请统一通过该脚本创建。

### PowerShell 调用示例

```powershell
python .\scripts\create_adr.py . "决策标题" --slug "short-slug" --status active --context "决策促成的背景、约束与痛点" --decision "最终确认的技术选择" --option "候选方案 A" --option "候选方案 B" --consequence "正向影响与需要承受的代价"
```


### Bash 调用示例

```bash
python3 ./scripts/create_adr.py . "决策标题" --slug "short-slug" --status active --context "决策促成的背景、约束与痛点" --decision "最终确认的技术选择" --option "候选方案 A" --option "候选方案 B" --consequence "正向影响与需要承受的代价"
```

### 参数规则

- `--adr-dir`：ADR 相对目录，默认为 `docs/adr`（可省略）。
- `--slug`：文件名短标识，中文标题必须显式传入 ASCII 格式的 slug。
- `--status`：初始状态，默认为 `proposed`；已落地的既定架构决策应传入 `--status active`。
- `--supersedes`：当新决策替代旧决策时，传入被替代 ADR 的四位编号（如 `--supersedes 0001`）。

## ADR 结构模板

```markdown
# NNNN — 决策标题

- Status: active
- Date: YYYY-MM-DD
- Supersedes: none

## Context

促成决策的业务背景、硬件环境约束与技术痛点。

## Decision

明确的技术路线选择与核心设计。

## Options considered

列出的各备选方案及其对比取舍。

## Consequences

- 正面影响与带来的优势。
- 付出的成本、引入的约束或后续重估触发条件。
```
