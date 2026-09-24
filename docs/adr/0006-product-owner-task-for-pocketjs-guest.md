# 0006 — 由产品 owner task 承载 PocketJS guest 生命周期

- 状态: retired
- 日期: 2026-09-11
- 替代: 无

## 背景

QuickJS 的栈守卫判据是 stack_top 减去 stack_size，其中 stack_top 取自创建 runtime 的那个任务，官方组件与仓库都没有调用 JS_UpdateStackTop。pocketjs_guest 默认把 stack_limit 设为 256 KB，而官方可选的 pocketjs_runner 只能指定任务栈大小，任务栈始终由 IDF 从内部 RAM 分配。Vue Vapor 应用的 mount 是深层递归，实测每嵌套一层 UI 约走 15 个 JS 帧、消耗约 1 KB 的 C 栈，示例界面 mount 需要 60 KB 以上。内部 RAM 无法提供这个连续空间，导致守卫失效并写穿任务栈、破坏相邻堆元数据，表现为位置漂移的崩溃。

## 决策

由产品自己在 firmware/main/pocketjs_host.c 中创建 remapad-pjs owner task，用 xTaskCreatePinnedToCoreWithCaps 把 288 KB 栈分配在 PSRAM，并由这一个任务依次完成 package、guest、ui_core、binding、mount、eval、renderer 的创建与逐帧 UI turn；guest 的 stack_limit 收敛到官方默认的 256 KB。官方 pocketjs_runner 保留在 firmware/components/ 内但不再接入。

## 考虑的方案

- 继续使用官方 pocketjs_runner：保留官方示例结构，但任务栈只能从内部 RAM 分配，且 mount 发生在 runner 启动之前，守卫基准与实际执行栈必然不一致
- 继续使用 pocketjs_runner 并调大 stack_limit：不解决任务栈容量问题，栈预算越大守卫反而越不可达
- 产品自建 owner task 并把栈放在 PSRAM：栈容量可控、创建与 turn 同任务，代价是自行维护 tick 循环与停止超时逻辑

## 影响

- 消除静默的栈溢出：设备从启动即崩溃变为稳定运行
- 内部 RAM 释放：主任务栈回到 32 KB，启动后内部可用约 360 KB，留给 DMA 缓冲与协议栈
- 自建 tick 循环需要自行维护：错过 deadline 的追帧与停止超时逻辑不再由官方组件提供
- 创建 guest 与执行 UI turn 必须是同一个任务：破坏该前提会让栈守卫重新失效，这是后续调度改动的硬约束
- 每帧 JS turn 与渲染合计接近整个 tick 周期，CPU 需运行在 240 MHz；若后续把 USB 或 BLE 数据面加到同一核需要重新评估
