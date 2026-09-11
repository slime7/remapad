# Remapad 目标硬件参考

本文档记录 Remapad 目标板卡的硬件事实：SoC 与存储、屏幕、触摸、其他板载外设、GPIO 分配，以及实机验证过的启动事实。面板、触摸与背光 BSP 已接入固件（见 [ADR 0007](adr/0007-esp-lcd-panel-touch-bsp.md)）；USB 输入、BLE、电池、IMU、RTC 与蜂鸣器尚未实现，这些外设仍只有硬件事实。

板卡为微雪 (Waveshare) **ESP32-S3-Touch-LCD-1.69**，SKU 27350；本文档的规格、引脚与地址来自微雪官方文档 <https://docs.waveshare.net/ESP32-S3-Touch-LCD-1.69>。

## SoC 与存储

| 项目 | 事实 | 来源 |
| :--- | :--- | :--- |
| 模组 | ESP32-S3R8，Xtensa LX7 双核，最高 240 MHz | 微雪文档 / 实机 |
| 片内 SRAM | 512 KB | 微雪文档 |
| PSRAM | 8 MB Octal，叠封在 SoC 内（AP Memory） | 微雪文档 / 实机 |
| Flash | 16 MB，W25Q128JVSIQ | 微雪文档 / 实机 |
| 无线 | 2.4 GHz Wi-Fi (802.11 b/g/n)、Bluetooth 5 (LE) | 微雪文档 |
| 天线 | 板载贴片天线 | 微雪文档 |

本仓库早期文档把硬件写成“ESP32-S3-WROOM-1 N16R8”模组。实际板卡使用 **ESP32-S3R8**，8 MB PSRAM 叠封在 SoC 内，不是模组外挂。内存结论（16 MB Flash + 8 MB Octal PSRAM）不变，`firmware/sdkconfig.defaults` 的 Flash/PSRAM 预设因此仍然正确。

实机启动日志（ESP-IDF v6.1）确认：

```text
Chip type:          ESP32-S3 (QFN56) (revision v0.2)
Features:           Wi-Fi, BT 5 (LE), Dual Core + LP Core, 240MHz, Embedded PSRAM 8MB (AP_3v3)
Crystal frequency:  40MHz
USB mode:           USB-Serial/JTAG
MAC:                28:84:85:55:41:e0
octal_psram: vendor id    : 0x0d (AP)
esp_psram: Found 8MB PSRAM device
spi_flash: detected chip: generic
```

`Embedded PSRAM` 与 `vendor id 0x0d (AP)` 说明 PSRAM 是叠封件。`CONFIG_SPIRAM_MODE_OCT` 必须保持开启：这块板在 Quad 模式下 PSRAM 无法初始化，官方示例默认的 Quad 预设不能直接照搬。

CPU 频率默认值是 160 MHz，SoC 支持 240 MHz。当前固件显式配置为 240 MHz，理由见 [ARCHITECTURE.md](ARCHITECTURE.md) 的性能预算说明。

## 屏幕与触摸

| 模块 | 器件 | 接口 | 关键参数 | GPIO |
| :--- | :--- | :--- | :--- | :--- |
| LCD | ST7789V2 | 4-wire SPI | 240 × 280，RGB565 | DC=GPIO4, CS=GPIO5, CLK=GPIO6, DIN=GPIO7, RST=GPIO8, BL=GPIO15 |
| 触摸 | CST816T | I2C | 7-bit 地址 `0x15` | SCL=GPIO10, SDA=GPIO11, RST=GPIO13, INT=GPIO14 |

显示视口与 `firmware/pocket.host.json` 的 `240 × 280` 逻辑/物理视口一致，也和 9 位触摸坐标契约（每轴 512 像素以内）兼容。

面板接线要点：

- **LCD 只有写入数据线**：`DIN=GPIO7` 连接面板数据输入，`LCD_DOUT` 未引出，因此不需要 MISO。面板驱动按只写 SPI 实现。
- **背光独立控制**：`BL=GPIO15` 不是自动点亮；面板 BSP 必须显式驱动该脚，否则即使成功提交帧也看不到画面。
- **触摸与 IMU、RTC 共享同一条 I2C**（GPIO10/GPIO11），三者挂在一个总线上，靠地址区分。

## 其他板载外设

| 模块 | 器件 / 功能 | 接口 | 地址 / 参数 | GPIO |
| :--- | :--- | :--- | :--- | :--- |
| IMU | QMI8658C 六轴（3 轴陀螺仪 + 3 轴加速度计） | I2C | 7-bit 地址 `0x6B` | SCL=GPIO10, SDA=GPIO11, INT=GPIO38 |
| RTC | PCF85063ATL | I2C | 7-bit 地址 `0x51`，32.768 kHz 晶振 | SCL=GPIO10, SDA=GPIO11, INT=GPIO39 |
| 蜂鸣器 | 板载蜂鸣器 | GPIO / PWM | tone / PWM 输出 | GPIO42 |
| 电池采样 | B+ 分压到 ADC | ADC | R3 上拉 200K、R7 下拉 100K；`VBAT = VADC × 3` | GPIO1 / BAT_ADC |
| 电源控制 | SYS_OUT / SYS_EN | GPIO | PWR / Key2 电源功能电路 | SYS_OUT=GPIO40, SYS_EN=GPIO41 |
| 充电管理 | ETA6098 | 电源 | 单节锂电池充放电 | 电池接口 MX1.25 2P |
| 3.3 V LDO | ME6217C33M5G | 电源 | 系统 3.3 V | VCC3V3 |
| USB Type-C | ESP32-S3 原生 USB | USB | 烧录与日志 | USB_N=GPIO19, USB_P=GPIO20 |
| UART0 | 默认串口 | UART | 调试 / 扩展焊盘 | U0TXD=GPIO43, U0RXD=GPIO44 |

IMU 中断脚在微雪文档内部存在一处不一致：外设速查表写 `INT1=GPIO38`，GPIO 分配表写 `GPIO38 = QMI_INT2`（`INT2`）。两条记录指向同一个 GPIO，但中断编号不同。接入 IMU 中断前应以原理图或实机读寄存器确认，不要直接照抄。

## GPIO 分配

| GPIO | 信号名 | 连接到 | 备注 |
| :--- | :--- | :--- | :--- |
| GPIO0 | BOOT / Key1 | BOOT 按键 | Strapping pin，长按上电再松开进入下载模式 |
| GPIO1 | BAT_ADC | 电池电压分压采样 | `VBAT = VADC × 3` |
| GPIO2 | GPIO2 | 预留焊盘 / 排针 | 扩展口 |
| GPIO3 | GPIO3 | 预留焊盘 / 排针 | 扩展口 |
| GPIO4 | LCD_DC | ST7789V2 数据/命令 | - |
| GPIO5 | LCD_CS | ST7789V2 片选 | - |
| GPIO6 | LCD_CLK | ST7789V2 SPI 时钟 | - |
| GPIO7 | LCD_DIN | ST7789V2 SPI 数据 | 仅写入方向，LCD_DOUT 未使用 |
| GPIO8 | LCD_RST | ST7789V2 复位 | - |
| GPIO10 | ESP32_SCL | 触摸 + IMU + RTC 共享 I2C SCL | 接出到扩展口 |
| GPIO11 | ESP32_SDA | 触摸 + IMU + RTC 共享 I2C SDA | 接出到扩展口 |
| GPIO13 | TP_RST | CST816T 触摸复位 | - |
| GPIO14 | TP_INT | CST816T 触摸中断 | - |
| GPIO15 | LCD_BL | LCD 背光控制 | 需要显式驱动 |
| GPIO17 | GPIO17 | 预留焊盘 / 排针 | 扩展口 |
| GPIO18 | GPIO18 | 预留焊盘 / 排针 | 扩展口 |
| GPIO19 | USB_N | USB Type-C D- | ESP32-S3 原生 USB |
| GPIO20 | USB_P | USB Type-C D+ | ESP32-S3 原生 USB |
| GPIO38 | QMI_INT2 | QMI8658C 中断 | 编号与外设速查表不一致，见上节 |
| GPIO39 | RTC_INT | PCF85063 中断 | - |
| GPIO40 | SYS_OUT | 系统电源控制网络 | PWR / Key2 电路 |
| GPIO41 | SYS_EN | 系统电源控制网络 | PWR / Key2 电路 |
| GPIO42 | Buzz | 板载蜂鸣器驱动 | PWM / tone |
| GPIO43 | U0TXD | UART TX | 扩展口 |
| GPIO44 | U0RXD | UART RX | 扩展口 |

扩展口可用信号：`5V` / `3V3` / `GND`、I2C（`GPIO10` / `GPIO11`）、UART（`GPIO43` / `GPIO44`），以及 `GPIO2` / `GPIO3` / `GPIO17` / `GPIO18`。

## 板级注意事项

- **I2C 地址冲突**：板内已占用 `0x15`（触摸）、`0x6B`（IMU）、`0x51`（RTC）。外接 I2C 设备必须避开这三个地址。
- **USB 口只有一个**：Type-C 直接连在 ESP32-S3 原生 USB（GPIO19/20）上，烧录、日志、以及未来的 USB 输入共用同一个物理口。当前实机以 `USB-Serial/JTAG` 模式枚举。产品数据面若要通过该口接收 USB 输入设备，需要这块口工作在 host 模式，届时会与串口调试通道互斥；具体方案留到产品 BSP 阶段确定并实测，不要在文档里预设已经可用。
- **`GPIO19` / `GPIO20`** 已接 Type-C，不要当普通 GPIO 使用。
- **`GPIO0` 是 BOOT**、`CHIP_PU` 是复位信号，都不适合作为普通用户输入。
- **按键资源**：`BOOT`(GPIO0)、`RST`(CHIP_PU)、`PWR`(SYS_OUT=GPIO40 / SYS_EN=GPIO41)。PWR 键支持上电检测、单击、双击、多击和长按，属于电源功能电路，接入前要确认它不会切断系统供电。

## 产品 BSP 尚未实现的范围

面板、触摸与背光已接入固件：`firmware/main/drivers/` 中的 `panel.c`（esp_lcd 内置 ST7789 驱动，SPI2 40 MHz）、`touch.c`（Registry 组件 `esp_lcd_touch_cst816s`，I2C `0x15`）与 `backlight.c`（GPIO15 LEDC PWM）承担面板初始化、strip 提交、触点采样和背光驱动；选型与取舍见 [ADR 0007](adr/0007-esp-lcd-panel-touch-bsp.md)。以下外设目前只有硬件事实，固件没有接入：

- 电池 ADC 采样与充电状态（`drivers/battery.c` 仍为占位，未编译）；
- IMU、RTC、蜂鸣器的驱动与状态上报；
- USB host 输入接收与 NS2 报告编码；
- BLE 广播、GATT 与配对状态机。

屏幕事实已写入 `firmware/pocket.host.json`：`input.touch` 随触摸采样接入一并声明。
