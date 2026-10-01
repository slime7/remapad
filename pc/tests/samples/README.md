# 测试样本：主机输出的原始采集

本目录存放主机输出的原始报文采集，用于离线测试与手柄回放：
- [ns2-search-page.capture](ns2-search-page.capture): 搜索页 20 秒报文。
- [ns2-gameplay-rumble.capture](ns2-gameplay-rumble.capture): 游戏内 40 秒震动报文。

## 回放到手柄（pad_replay.py）

将 `.capture` 样本按转换规则回放到 DualSense 手柄：

```powershell
uv run python pc/tests/samples/pad_replay.py --list
uv run python pc/tests/samples/pad_replay.py ns2-search-page.capture --pad usb   # HD 全保真（WASAPI 4ch）
uv run python pc/tests/samples/pad_replay.py ns2-gameplay-rumble.capture --pad bt --speed 2
uv run python pc/tests/samples/pad_replay.py ns2-gameplay-rumble.capture --pad bt32 --hd-gain 1
```

回放目标（`--pad`）：
- `usb`：直插 DualSense 音频触觉流（WASAPI 4ch）。
- `bt`：蓝牙传统双马达近似（0x31 报告）。
- `bt32`：蓝牙私有触觉流（发声段折入音圈）。
- `bt36`：蓝牙触觉与 Opus 喇叭流（需 PyAV/libopus）。
- `bt39`：蓝牙成对触觉与喇叭流。

## 文件格式

一行一条记录（UTF-8 文本，LF），`#` 开头是头注释：

```text
+0.500s rumble[0x12] seq=000  32B 7c 04 80 01 97 63 ...
```

列字段：相对时间戳、通道名 `[GATT 句柄]`、记录序号、数据长度、原始十六进制报文。

## 重新抓取

在游戏过程中采集原始下行数据：

```powershell
uv run python pc/ctrl.py -p COM12 --pad --capture pc/tests/samples/ns2-host-output.capture --seconds 20
```
