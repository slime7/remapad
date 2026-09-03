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

下表是本目录的完整 ADR 索引，也是当前决策状态的唯一导航入口。新增 ADR 后必须追加对应行，并继续保持编号升序；状态变更时只修改状态列和必要的取代说明。

| ADR | 状态 | 主题 |
| --- | --- | --- |
| [0001](0001-use-pocketjs-vue-vapor-for-esp32s3-ui.md) | active | 采用 PocketJS 与 Vue Vapor 驱动 ESP32-S3 屏幕 UI |
| [0002](0002-adopt-hardware-bridge-and-packaging-architecture.md) | active | 引入统一硬件桥接协议与双工作区分层架构；打包部分由 0003 取代 |
| [0003](0003-use-official-esp-idf-host.md) | active | 采用官方 PocketJS ESP-IDF host 构建链路 |

`0002` 仍作为硬件 bridge 与控制面分层的决策依据；其自定义打包和 host 接入范围由 `0003` 取代。旧 ADR 文件正文保持不变。

## 创建 ADR 脚本用法

项目已内置 ADR 生成脚本 [scripts/create_adr.py](../../scripts/create_adr.py)，基于 Python 3.10+ 运行。不要手动编写或硬编码文件编号，请统一通过该脚本创建。

### PowerShell 调用示例

```powershell
python .\scripts\create_adr.py F:/private/remapad "决策标题" --slug "short-slug" --status active --context "决策促成的背景、约束与痛点" --decision "最终确认的技术选择" --option "候选方案 A" --option "候选方案 B" --consequence "正向影响与需要承受的代价"
```


### Bash 调用示例

```bash
python3 ./scripts/create_adr.py F:/private/remapad "决策标题" --slug "short-slug" --status active --context "决策促成的背景、约束与痛点" --decision "最终确认的技术选择" --option "候选方案 A" --option "候选方案 B" --consequence "正向影响与需要承受的代价"
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
