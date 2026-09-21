# 0010 — BLE 手柄外设采用 NimBLE 栈

- 状态: active
- 日期: 2026-09-12
- 替代: 无

## 背景

Phase 2 进入 BLE 手柄外设实现。ESP32-S3 只支持 Bluetooth LE 5.x、无经典蓝牙控制器；
参考实现（见 [controller-switch2.md](../controller-switch2.md)）基于 NimBLE，社区 Switch 2 模拟实践亦然。
NS2 使用自定义配对协议，安全由应用层 Command 0x15 配对承担；连接间隔需接受主机（central）主导的 5-10ms 区间；
固件还需与 PocketJS guest（4MB JS 堆）共存，内部 RAM 空闲约 360KB，栈与堆预算紧张。
后续实机对账补充：主机在 MTU 交换后会先走标准 BLE SMP（Just Works，仅分发 ENC 密钥），
实现因此接受标准 SMP 并用 0x15 的结果注入绑定键；本条决策里「收到 SMP 请求按协议拒绝」的处理已被取代，其余各项继续生效。

## 决策

采用 ESP-IDF 内置 NimBLE host + BLE-only controller：不启用 Bluedroid；SMP 保持关闭，不主动发起安全请求，收到主机 SMP 请求按协议拒绝；广播由应用层构造 31 字节原始 ADV 数据（Flags + 26 字节厂商数据）；连接参数接受主机请求并约束在 5-10ms 区间；NimBLE 主机内存优先分配到 PSRAM（CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL），保障 guest 堆与内部 RAM 预算。

## 考虑的方案

- NimBLE：BLE-only 轻量主机，参考实现同栈，SMP 处理路径清晰（采纳）
- Bluedroid：ESP-IDF 双栈主机，资源占用更大，本场景无经典蓝牙需求（否决）
- 自研或第三方 BLE 主机栈：成本与风险不可行（否决）

## 影响

- 正面：内存占用小、与协议参考实现同栈、SMP 禁用路径清晰、实机对照资料充足。
- 约束：NimBLE 版本随 ESP-IDF 6.x 内置版本锁定；连接间隔实际由主机主导，需实机验证外设侧接受策略；31 字节广播无名称字段余量，设备识别完全依赖厂商数据。
