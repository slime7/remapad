# 测试样本：主机输出的原始采集

本目录存放经桥接帧 `0x12`（HOST_RAW）从真机抓到的主机输出原始数据，供 PC 侧
测试在无设备的情况下回放（例如 NS 主机输出 → DS5 手柄输出报告的转换用例）。

- **[ns2-search-page.capture](ns2-search-page.capture)**: NS2 主机 20 秒的输出原文。
- **[ns2-gameplay-rumble.capture](ns2-gameplay-rumble.capture)**: NS2 主机 40 秒的游戏内输出原文，
  PC 侧转发 USB DualSense 游玩期间抓取：连接期 3 条 `composite[0x16]`（采样 ID 0x00 的停止帧），
  进游戏后 `rumble[0x12]` 以约 66-190 Hz 密集出现（42 字节 = `0x00` 占位 + 左右 LRA 参数包 + 9B 保留，
  超过一半是零幅度保活包），3 处跳号是抓取期间设备队列满丢包。

## 回放到手柄（pad_replay.py）

[pad_replay.py](pad_replay.py) 把 .capture 样本按固件同一套转换规则回放成
DualSense 的触觉/声音，也提供分段测试包（A = 0x31 双马达、B = 0x32 HD 触觉、
C = 0x32 两声上行短鸣），用于核对 Windows 各条写回通路：

```powershell
cd pc
uv run python tests/samples/pad_replay.py --list
uv run python tests/samples/pad_replay.py ns2-search-page.capture --pad usb   # HD 全保真（WASAPI 4ch）
uv run python tests/samples/pad_replay.py ns2-gameplay-rumble.capture --pad bt --speed 2
uv run python tests/samples/pad_replay.py --test                              # 蓝牙分段写回探测
```

落点：`usb` = 直插 DS5 的音频触觉（HD 全保真）；`bt` = 蓝牙 0x31
双马达近似（HD 纹理压成两带、发声段丢弃）；`bt32` = 蓝牙 0x32 私有触觉流
（SAxense 142 字节原始形态直写，不填充——蓝牙描述符对 0x32 就声明 141 字节数据，
547 是同族 0x39 的长度，填充反而让手柄收不到），发声段折进音圈（摸得到、
听不到）；`bt36` = 蓝牙 0x36 私有触觉+喇叭流（vds 398 字节形态，发声段由手柄
喇叭真声播放，需要 PyAV/libopus，缺失回落 bt32）。
分段测试包（`--test`）依次发 A = 0x31 双马达、B = 0x32 HD 触觉、
C = 0x32 短鸣（折进音圈）、D = 0x36 短鸣（喇叭真声）。
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

进游戏触发震动时 `0x0012` 会以约 66 Hz 出现 `rumble[0x12]` 记录：NS2 实抓为 42 字节
（`0x00` 占位 + 左右两条 16 字节 LRA 参数包 + 9B 保留，见 [ns2-gameplay-rumble.capture](ns2-gameplay-rumble.capture)），
早期对账还记录过不带前缀的 32 字节形态，位布局见 controller.md「输出报告格式」的勘误表。

## 重新抓取

主机输出什么就抓到什么：要采到震动参数包（`rumble[0x12]`）与玩家灯命令
（`cmd[0x14]`），在主机进游戏并触发震动的期间执行：

```powershell
cd pc
uv run python remapadctl.py -p COM12 --pad --capture tests/samples/ns2-host-output.capture --seconds 20
```
