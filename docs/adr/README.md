# 架构决策记录 (ADR)

本目录记录 Remapad 项目中具有长期影响的重要架构决策、技术选型背景及其取舍权衡。

## 为什么需要 ADR

架构决策记录（Architecture Decision Record, ADR）捕获决策做出时的具体背景、考虑的候选方案及正面与负面影响，为后续维护团队和 coding agent 提供权威的决策依据，防止陷入重复推翻已有架构权衡的循环。

## 文件与状态规范

- **文件命名**：采用 `NNNN-slug.md` 格式，编号从 `0001` 开始单调递增，四位数字对齐。
- **状态流转**：
  - `proposed`：决策正在提议或评审中。
  - `active`：决策已被批准并作为当前生效的架构基准。
  - `superseded`：决策已被新的 ADR 取代（必须通过 `Supersedes` 标记原决策并链接到新决策）。
  - `retired`：决策所针对的功能或组件已被废弃淘汰。
- **决策边界**：一份 ADR 仅专注记录一项具体的长期决策。日常的缺陷修复、小范围样式微调或无重大架构影响的重构无需编写 ADR。

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

