# 测试样本：主机输出的原始采集

本目录存放经桥接帧 `0x12`（HOST_RAW）从设备抓到的主机输出原始数据，供 PC 侧
测试在无设备的情况下回放（例如 NS 主机输出 → DS5 手柄输出报告的转换用例）。

- **[ns2-search-page.capture](ns2-search-page.capture)**: NS2 主机 20 秒的输出原文。
- **[ns2-gameplay-rumble.capture](ns2-gameplay-rumble.capture)**: NS2 主机 40 秒的游戏内输出原文，
  PC 侧转发 USB DualSense 游玩期间抓取：连接期 3 条 `composite[0x16]`（采样 ID 0x00 的停止帧），
  进游戏后 `rumble[0x12]` 以约 66-190 Hz 密集出现（42 字节 = `0x00` 占位 + 左右 LRA 参数包 + 9B 保留，
  超过一半是零幅度保活包）。

## 回放到手柄（pad_replay.py）

[pad_replay.py](pad_replay.py) 把 .capture 样本按固件同一套转换规则回放成
DualSense 的触觉/声音：

```powershell
cd pc
uv run python tests/samples/pad_replay.py --list
uv run python tests/samples/pad_replay.py ns2-search-page.capture --pad usb   # HD 全保真（WASAPI 4ch）
uv run python tests/samples/pad_replay.py ns2-gameplay-rumble.capture --pad bt --speed 2
uv run python tests/samples/pad_replay.py ns2-gameplay-rumble.capture --pad bt32 --hd-gain 1
```

落点：`usb` = 直插 DS5 的音频触觉（HD 全保真）；`bt` = 蓝牙 0x31
双马达近似（HD 纹理压成两带、发声段丢弃，两颗马达按固件的分带与感知曲线驱动）；
`bt32` = 蓝牙 0x32 私有触觉流
（SAxense 142 字节原始形态直写，不填充——蓝牙描述符对 0x32 就声明 141 字节数据，
547 是同族 0x39 的长度，填充反而让手柄收不到），发声段折进音圈（摸得到、
听不到）；`bt36` = 蓝牙 0x36 私有触觉+喇叭流（vds 398 字节形态，发声段由手柄
喇叭真声播放，需要 PyAV/libopus，缺失回落 bt32）；`bt39` = 蓝牙 0x39 成对形态
（一报 2 块触觉 + 2 帧喇叭、节拍 21.33ms，多带的那一块是链路抖动的水垫——
单块形态下一拍迟到 10.67ms 就断音，成对形态的容差翻倍、报数减半）。
`auto` 与产品通路一致：直插 DS5 在就落 `usb`，否则落蓝牙私有触觉流（有
PyAV/libopus 走 `bt36`，缺了退 `bt32`），两端都不可用才回落到 `bt` 的双马达
近似；跑起来先打印实际落点，想复现回落的症状才要显式给 `--pad bt`。
两个私有流落点开流前先写一份 0x31 喇叭路由与音量档
（输出路径 = 手柄喇叭、前级 +6dB、音量 100）：不路由时手柄内置喇叭一声不出；
`usb` 落点同样先经 HID 写一份 0x02 的同款预置。
私有流写回被拒（句柄失效/链路不接受）时提示并回落 `bt` 的 HID 震动，不让整次
回放崩掉；连接方式按 HID 的 `bus_type` 判定（0x0DF2 同时是 DualSense Edge 的
有线 PID，按 PID 猜会把直插的 Edge 当成蓝牙）。
回放按主机声明的子帧数轮播（`FeedbackSim` 与固件同一套语义）：实抓的游戏流每包
只声明 1 个子帧，声明之外的槽位不占时间——固定按 3 槽轮播会把持续震动切成 66Hz 断续。
`--hd-gain` 是触觉增益倍率（只抬振幅、不动频率与段边界），默认取布局行 `hd` 规则的标定值
（4 倍，见 [../../../docs/controller-ps.md](../../../docs/controller-ps.md) 的波形重整规则）；
传 `--hd-gain 1` 回原始刻度做对比。
各蓝牙落点回放结束会打印写回耗时统计（平均/最大单次耗时、超节拍份数）：
平均越接近节拍说明链路越撑得住，明显超节拍说明报文被排队、触觉/声音会延迟。
回放中 Ctrl-C 随时干净退出（音频流与 HID 句柄都会收尾）。

## 文件格式

一行一条记录（UTF-8 文本，LF），`#` 开头是头注释：

```text
+0.500s rumble[0x12] seq=000  32B 7c 04 80 01 97 63 ...
```

列依次是：相对抓取起点的秒数、通道名 `[GATT 句柄低字节]`（与固件
`dp_capture.h` / `pc/link.py` 的 HOST_RAW_CHANNELS 同一张表）、设备侧记录号
（跳号 = 抓取期间设备队列满丢包）、数据字节数（`trunc` 标记 = 超过单帧上限
被截断）、原始字节的十六进制。

进游戏触发震动时 `0x0012` 会以约 66 Hz 出现 `rumble[0x12]` 记录：NS2 为 42 字节
（`0x00` 占位 + 左右两条 16 字节 LRA 参数包 + 9B 保留，见 [ns2-gameplay-rumble.capture](ns2-gameplay-rumble.capture)），
位布局见 [../../../docs/controller-switch2.md](../../../docs/controller-switch2.md) 的输出报告一节。

## 重新抓取

主机输出什么就抓到什么：要采到震动参数包（`rumble[0x12]`）与玩家灯命令
（`cmd[0x14]`），在主机进游戏并触发震动的期间执行：

```powershell
cd pc
uv run python remapadctl.py -p COM12 --pad --capture tests/samples/ns2-host-output.capture --seconds 20
```
