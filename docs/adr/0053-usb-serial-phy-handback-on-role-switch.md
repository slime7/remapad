# 0053 — host 切回串口时显式交还内部 PHY，失败由界面询问重启

- 状态: active
- 日期: 2026-09-22
- 替代: 无

## 背景

USB-Serial/JTAG 与 OTG host 复用 GPIO19/20 上唯一的内部 FSLS PHY，切到 host 后 PC 上的 COM 口消失。
ADR 0027 定下了切到 host 的做法与「复位是保底恢复路径」，但没有做切回。
usb_host_uninstall() 只调 usb_del_phy() 清上下拉与焊盘、不翻复用开关（IDF v6.1 源码事实，见 docs/hardware.md）。
运行时切回串口后 COM 口不会回来，模式页的「串口」卡片会变成假开关，只能靠重启恢复。

## 决策

切回 device 时固件用 usb_new_phy(USB_PHY_CTRL_SERIAL_JTAG, USB_PHY_TARGET_INT) 把内部 PHY 指回 USB-Serial/JTAG，并持有句柄。
进 host 前先 usb_del_phy() 放掉同一块 PHY，否则 host 栈安装会因 selected PHY is in use 失败。
交还失败不阻塞切回：界面在切回成功后询问是否立刻重启设备，复位始终是保底恢复路径。

## 考虑的方案

- 运行时交还内部 PHY，失败由界面询问重启（采纳）
- 只让复位恢复串口（否决：模式页的「串口」卡片会变成假开关）
- 切回串口后自动重启（否决：切回常常只为继续当前会话，重启会打断 BLE 链路与手上的游戏）
- 绕开官方驱动直接翻复用开关位（否决：IDF 升级易碎，usb_phy 已经封装这条路）

## 影响

- 正面：切回串口后 COM 口即时回来，桥接、烧录与 OTA 不必重启即可继续；重启询问兜住交还失败的情况。
- 约束：串口 PHY 句柄由 usb/usb_role.c 持有，进 host 前必须先放掉，否则 usb_host_install 报 selected PHY is in use。
- 约束：host 模式下的日志与 CLI 仍只有 UART0（GPIO43/44）一条通道，切回后也补看不到 host 期间的历史日志。
- 门禁：mux 切换、PHY 交还与手柄枚举仍需实机核对，结论回填 docs/hardware.md 的「USB 控制器复用」。
