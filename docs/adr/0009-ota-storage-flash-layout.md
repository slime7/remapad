# 0009 — 固化 16MB Flash 分区终局布局：OTA 双分区与通用存储区

- 状态: active
- 日期: 2026-09-12
- 替代: 无

## 背景

固件路线图包含 BLE 配对、用户设置（亮度、连发/改建、手柄颜色）与用户上传 amiibo（NTAG215 二进制），并保留将来通过 USB/BLE 进行 OTA 升级的可能。原分区表仅有 nvs、phy_init 与 4MB factory，16MB Flash 尾部约 11.7MB 未分配；这些能力一旦在设备部署后引入都必须改动分区表，而布局变化会连带擦除已写入的用户数据（NVS 配对密钥、storage 分区内容）。当前设备尚无任何持久化数据，是冻结分区布局的最低成本时机。

## 决策

16MB Flash 一次性划分为 nvs(0x9000, 24K)、phy_init(0xf000, 4K)、ota_0(0x10000, 4M)、ota_1(0x410000, 4M)、otadata(0x810000, 8K)、storage(0x812000, 约 7.9M，subtype spiffs)。nvs 与 phy_init 偏移与旧表保持一致，保证后续固件升级永不擦除用户 NVS 数据；ota_0 继承原 factory 偏移 0x10000，已部署设备只需重写分区表即可原地迁移（otadata 为空时 bootloader 回退 ota_0），开发期 app-flash 写 0x10000 的习惯不变；otadata 放在 ota_1 之后而非 phy_init 旁，正是为了保住该继承路径。小配置（设置项与 BLE 配对密钥）一律存 NVS；大块用户数据存通用命名的 storage 分区（不把分区名绑定到具体数据类型，首个用途是用户上传的 NTAG215 amiibo 二进制），将来按分区名挂 littlefs（subtype 不限制文件系统选型）。本次仅划分区，不引入 OTA 或文件系统代码。长期约束：新增分区只允许在尾部追加，禁止移动 nvs/phy_init 偏移；开发期烧录用 app-flash，禁止随手 erase-flash。

## 考虑的方案

- 保留 factory 并追加 ota_0/ota_1 三分区：保留出厂回退镜像，但 factory 的 4MB 在首次 OTA 后永久闲置，浪费四分之一 Flash。
- 教科书布局（otadata 紧跟 phy_init，ota_0 从 0x20000 开始）：分区顺序更常规，但 0x10000 处的已部署固件必须整体重刷，放弃只写分区表的最小迁移路径。
- 维持现状（仅 factory），需要时再改：短期最省事，但首个用户数据落地后每次分区调整都危及数据，迁移成本随部署量单调增长。

## 影响

- 正面：OTA 能力将来可直接使用 esp_ota API 而无需再动分区表；storage 分区约 7.9MB，可存上万个 NTAG215 镜像（单个 540B）并为其他用户数据预留空间；已部署实机迁移只写 0x8000 处一个 4KB 分区表扇区。
- 代价：应用空间从单 4MB 变为双 4MB 互备（当前镜像约 2.25MB，余量充足）；otadata 位置偏离惯例，需借助分区表注释与本 ADR 理解；一旦将来 OTA 运行期切到 ota_1，开发期固定写 0x10000 的 app-flash 所刷的 ota_0 未必是被启动的分区，需先擦除 otadata 再继续开发烧录。
