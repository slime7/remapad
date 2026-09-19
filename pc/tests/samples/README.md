# 测试样本：主机输出的原始采集

本目录存放经桥接帧 `0x12`（HOST_RAW）从真机抓到的主机输出原始数据，供 PC 侧
测试在无设备的情况下回放（例如 NS 主机输出 → DS5 手柄输出报告的转换用例）。

- `ns2-host-output.capture`：NS2 主机 20 秒的输出原文（2026-09-20 实机抓取，
  223 条记录）。主机当时在「查找手柄」页，内容是复合通道（`0x16`）上的
  Command 0x0A 触觉采样流（采样 `0x02` 的启停交替）与 33 字节零前缀。

## 文件格式

一行一条记录（UTF-8 文本，LF），`#` 开头是头注释：

```text
+0.500s rumble[0x12] seq=000  32B 7c 04 80 01 97 63 ...
```

列依次是：相对抓取起点的秒数、通道名 `[GATT 句柄低字节]`（与固件
`dp_capture.h` / `pc/link.py` 的 HOST_RAW_CHANNELS 同一张表）、设备侧记录号
（跳号 = 抓取期间设备队列满丢包）、数据字节数（`trunc` 标记 = 超过单帧上限
被截断）、原始字节的十六进制。

## 重新抓取

主机输出什么就抓到什么：要采到震动参数包（`rumble[0x12]`）与玩家灯命令
（`cmd[0x14]`），在主机进游戏并触发震动的期间执行：

```powershell
cd pc
uv run python remapadctl.py -p COM12 --capture tests/samples/ns2-host-output.capture --seconds 20
```
