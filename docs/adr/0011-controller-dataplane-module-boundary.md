# 0011 — 控制器数据面模块边界与任务契约

- 状态: active
- 日期: 2026-09-12
- 替代: 无

## 背景

Phase 2 落地最终目标的 USB 输入到 NS2 报告再到 BLE 手柄数据面。ARCHITECTURE.md 不变量要求高频路径与 PocketJS turn 及 JSON bridge 解耦，bridge 只承载低频控制面；NS2 编码是纯软件枢纽可先行验证，USB host 与 BLE 外设需分别实机实验、风险与进度不同步；BLE 配对凭证按 ADR 0009 存 NVS。当前 firmware/main/ 仅有 bridge/ 与 drivers/，数据面缺整体结构约定。

## 决策

firmware/main/ 下按四模块划分数据面：ns2/（纯软件编码核心：规范化 controller state、Input Report 0x09/0x05 编码、指令帧与 0x15 配对帧构造）、ble/（NimBLE 手柄外设：广播、GATT、会话与配对状态机、notify 循环）、dp/（数据面任务：固定周期执行输入源采样、规范化、编码、BLE 提交；输入源抽象，当前接合成测试源，后续接 USB）、usb/（USB host：PHY 复用切换、HID 枚举接收解析、输出转发）。数据流单点汇合于 dp_task：输入源到 ns2_state 到 ns2_report 再到 BLE notify；主机下发的震动、LED 与指令经 ble 或 usb 反向转发。高频数据一律不经过 js_bridge。

## 考虑的方案

- 按 ns2/ble/dp/usb 四模块分层，编码核心独立于传输层（采纳）
- 单一大模块 controller/ 混编所有数据面代码：USB 与 BLE 无法独立验证与回退（否决）
- 数据面挂入 pocketjs_host owner task：违反高频路径与 UI 解耦的架构不变量，栈与实时性风险（否决）

## 影响

- 正面：NS2 编码核心可先行开发并独立验证，USB 与 BLE 各自独立实验；任务边界清晰，利于内存与实时性预算。
- 约束：跨任务队列与缓冲需要明确所有权与拷贝语义；新增数据面任务栈需纳入预算；模块间共享状态需避免锁争用。
