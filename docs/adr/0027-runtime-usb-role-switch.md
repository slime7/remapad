# 0027 — USB host 直插采用运行时角色切换，复位回到串口

- 状态: active
- 日期: 2026-09-15
- 替代: 无

## 背景

板卡只有一个 Type-C，USB-Serial/JTAG 与 USB OTG host 复用同一个片内 FSLS PHY，切到 host 后 PC 上的 COM 口消失。此前 usbRole 只记录角色、不触碰复用开关，手柄插在板卡上的数据面没有实现。

## 决策

切到「手柄」时按固定顺序执行：先把日志与 CLI 出口换到 UART0（GPIO43/44），再停掉桥接链路并卸 USB-Serial/JTAG 驱动，最后装 USB host 栈（安装时由 usb_phy 把复用开关切到 OTG host）。任何一步失败都回滚到串口角色。角色只对本次运行生效、不写 NVS，复位后复用开关回默认的 USB-Serial/JTAG，COM 口天然回来，reboot 是保底恢复路径。

## 考虑的方案

- 运行时切换、复位回串口（采纳）
- 重启进 host 模式并持久化角色（否决：恢复路径要额外状态，复位语义被打乱）
- 只在启动时按 CLI 参数决定角色（否决：现场改角色必须重新上电，屏幕 UI 用不上）

## 影响

- 正面：模式页的「手柄」卡片与 PWR 长按都能即时切角色，烧录链路不受影响（复位即回 COM）。
- 约束：host 模式下日志与 CLI 只在 UART0（GPIO43/44 扩展焊盘，需 USB-UART 适配器），PC 桥接与 OTA 在 host 模式不可用。
- 门禁：VBUS 5V 供电路径仍未确认，是 host 模式能否给插入手柄供电的前提，实机结论回填 hardware.md。
