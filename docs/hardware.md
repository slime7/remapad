# Remapad 目标硬件参考

本文档只记录目标板卡与 SoC 的硬件事实：SoC 与存储、屏幕与触摸、其他板载外设、GPIO 分配、USB 控制器与供电路径、板级约束。
固件如何驱动这些器件、链路上跑什么协议，见 [ARCHITECTURE.md](ARCHITECTURE.md) 与 [ABSTRACTIONS.md](ABSTRACTIONS.md)。
操作、命令与排错见 [GETTING-STARTED.md](GETTING-STARTED.md)。

板卡为微雪 (Waveshare) **ESP32-S3-Touch-LCD-1.69**，SKU 27350；
本文档的规格、引脚与地址来自微雪官方文档 <https://docs.waveshare.net/ESP32-S3-Touch-LCD-1.69>，标注「实机」的条目由本板启动日志与原理图核对。

## SoC 与存储

| 项目 | 事实 | 来源 |
| :--- | :--- | :--- |
| 模组 | ESP32-S3R8，Xtensa LX7 双核，最高 240 MHz | 微雪文档 / 实机 |
| 封装与版本 | QFN56，芯片版本 v0.2 | 实机 |
| 片内 SRAM | 512 KB | 微雪文档 |
| PSRAM | 8 MB Octal，叠封在 SoC 内（AP Memory，vendor id 0x0d） | 微雪文档 / 实机 |
| Flash | 16 MB，W25Q128JVSIQ | 微雪文档 / 实机 |
| 晶振 | 40 MHz | 实机 |
| 无线 | 2.4 GHz Wi-Fi (802.11 b/g/n)、Bluetooth 5 (LE) | 微雪文档 |
| 天线 | 板载贴片天线 | 微雪文档 |

PSRAM 是叠封在 SoC 内的 Octal 件（实机枚举为 `Embedded PSRAM`、vendor id 0x0d），Quad 模式下无法初始化。

## 屏幕与触摸

| 模块 | 器件 | 接口 | 关键参数 | GPIO |
| :--- | :--- | :--- | :--- | :--- |
| LCD | ST7789V2 | 4-wire SPI | 240 × 280，RGB565 | DC=GPIO4, CS=GPIO5, CLK=GPIO6, DIN=GPIO7, RST=GPIO8, BL=GPIO15 |
| 触摸 | CST816T | I2C | 7-bit 地址 `0x15` | SCL=GPIO10, SDA=GPIO11, RST=GPIO13, INT=GPIO14 |

面板接线要点：

- **LCD 只有写入数据线**：`DIN=GPIO7` 连接面板数据输入，`LCD_DOUT` 未引出，因此面板侧不需要 MISO。
- **背光独立控制**：`BL=GPIO15` 不是自动点亮，需要外部显式驱动该脚。
- **触摸与 IMU、RTC 共享同一条 I2C**（GPIO10/GPIO11），三者挂在一个总线上，靠地址区分。

## 其他板载外设

| 模块 | 器件 / 功能 | 接口 | 地址 / 参数 | GPIO |
| :--- | :--- | :--- | :--- | :--- |
| IMU | QMI8658C 六轴（3 轴陀螺仪 + 3 轴加速度计） | I2C | 7-bit 地址 `0x6B` | SCL=GPIO10, SDA=GPIO11, INT=GPIO38 |
| RTC | PCF85063ATL | I2C | 7-bit 地址 `0x51`，32.768 kHz 晶振 | SCL=GPIO10, SDA=GPIO11, INT=GPIO39 |
| 蜂鸣器 | 板载无源蜂鸣器 | GPIO / PWM | tone / PWM 输出 | GPIO42 |
| 电池采样 | B+ 分压到 ADC | ADC | R3 上拉 200K、R7 下拉 100K；`VBAT = VADC × 3`；引脚即 ADC1_CH0；实测 2.87–4.07 V（插电抬到 4.15 V） | GPIO1 / BAT_ADC |
| 电源控制 | SYS_OUT / SYS_EN | GPIO | PWR / Key2 电源功能电路 | SYS_OUT=GPIO40, SYS_EN=GPIO41 |
| 充电管理 | ETA6098 | 电源 | 单节锂电池充放电 | 电池接口 MX1.25 2P |
| 3.3 V LDO | ME6217C33M5G | 电源 | 系统 3.3 V | VCC3V3 |
| USB Type-C | ESP32-S3 原生 USB | USB | 片内 USB，复位后默认接 USB-Serial/JTAG | USB_N=GPIO19, USB_P=GPIO20 |
| UART0 | 默认串口 | UART | 调试 / 扩展焊盘 | U0TXD=GPIO43, U0RXD=GPIO44 |

IMU 中断脚在微雪文档内部存在一处不一致：外设速查表写 `INT1=GPIO38`，GPIO 分配表写 `GPIO38 = QMI_INT2`（`INT2`）。两条记录指向同一个 GPIO，但中断编号不同。
接入 IMU 中断前应以原理图或实机读寄存器确认，不要直接照抄。

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

对外焊盘按 V2.1 原理图核对分两处：

- **H1 排针（20 针）**：`3V3`（3 / 4 / 17 脚）、`GND`（1 / 5 / 6 / 12 / 18 / 19 / 20 脚）、
  I2C（13 脚 `GPIO10` SCL / 14 脚 `GPIO11` SDA）、LCD 控制线（2 脚 LEDK，7–11 脚 `GPIO4`–`GPIO8`）、
  触摸复位 / 中断（15 / 16 脚 `GPIO13` / `GPIO14`）；排针上没有 5V、UART 与 `GPIO2` / `GPIO3` / `GPIO17` / `GPIO18`。
- **TP1–TP11 测试点**：TP1=`VBUS`（5V）、TP2=`GND`、TP3=`3V3`、TP4 / TP5=UART0（`GPIO44` / `GPIO43`）、
  TP6 / TP7=I2C、TP8–TP11=`GPIO2` / `GPIO3` / `GPIO17` / `GPIO18`。

`5V` 即 TP1 上的 VBUS 网络，供电路径见「供电路径与 VBUS」。

## 板级注意事项

- **I2C 地址冲突**：板内已占用 `0x15`（触摸）、`0x6B`（IMU）、`0x51`（RTC）。外接 I2C 设备必须避开这三个地址。
- **USB 口只有一个**：Type-C 直接连在 ESP32-S3 原生 USB（GPIO19/20）上，烧录、日志与 USB 设备共用同一个物理口。
  复位后默认以 `USB-Serial/JTAG` 模式枚举，复用机制见「USB 控制器复用」。
- **Type-C 座子是纯 UFP 接线**：CC1 / CC2 各只接一颗 5.1 kΩ 下拉到地、没有 Rp 上拉，CC 也不连 ESP32（V2.1 原理图核对）。
  host 模式用 C-to-C 线直连手柄时，手柄在 CC 上看不到主机角色，VBUS 到位也只亮充电灯、数据不建立（实机已复现）；
  直插手柄要用的线序见 [GETTING-STARTED.md](GETTING-STARTED.md) 的「USB 手柄直插（host 模式）」。
- **`GPIO19` / `GPIO20`** 已接 Type-C，不要当普通 GPIO 使用。
- **`GPIO0` 是 BOOT**、`CHIP_PU` 是复位信号，都不适合作为普通用户输入。
- **按键资源**：`BOOT`(GPIO0)、`RST`(CHIP_PU)、`PWR`(SYS_OUT=GPIO40 / SYS_EN=GPIO41)。
  PWR 键支持上电检测、单击、双击、多击和长按，属于电源功能电路，接入前要确认它不会切断系统供电。
  电池供电时 SYS_EN 必须保持高电平锁存，否则松开 PWR 键即断电。

## USB 控制器复用

ESP32-S3 片内有两个 USB 控制器，共用 GPIO19/20 上唯一的内部 FSLS PHY（模拟收发前端），中间隔着一片片内复用开关，同一时刻只有一个控制器能接到物理口：

| 控制器 | 角色 | 用途 |
| :--- | :--- | :--- |
| USB-Serial/JTAG | 固定 device | 烧录与串口日志，实机枚举出的 COM 口就是它 |
| USB OTG 1.1 | device / host，全速 12 Mbps | 连接 USB 设备（手柄） |

复用开关由 `RTC_CNTL_USB_CONF` 寄存器控制（来源：IDF v6.1 `components/soc/esp32s3/register/soc/rtc_cntl_reg.h`）：

| 寄存器位 | 复位默认 | 含义 |
| :--- | :--- | :--- |
| `SW_HW_USB_PHY_SEL`（bit 20） | 0 | 是否启用软件控制复用开关 |
| `SW_USB_PHY_SEL`（bit 19） | 0 | 0 = PHY 接 USB-Serial/JTAG；1 = PHY 接 USB OTG |

芯片与 IDF v6.1 驱动的事实（来源：`components/esp_hal_usb/esp32s3/include/hal/usb_wrap_ll.h` 与
`components/esp_hw_support/include/esp_private/usb_phy.h`）：

- **复位默认永远接 USB-Serial/JTAG**：无论运行时把开关切到哪，每次上电/复位后 COM 口与 ROM 下载模式都恢复可用，烧录链路天然保留。
- **运行时切换是纯软件操作**：由 `usb_new_phy()` 指定 `controller = USB_PHY_CTRL_OTG` 与 `otg_mode = USB_OTG_MODE_HOST`，
  不需要直接写寄存器；切换后 PC 上的 COM 口消失。
- **切回串口**：复位即回默认位；不重启切回要重新初始化 PHY 并指定 `USB_PHY_CTRL_SERIAL_JTAG`，
  `usb_del_phy()` 只清理上拉与焊盘、不会把选择位翻回 USB-Serial/JTAG。

## 供电路径与 VBUS

- **板上没有电池 → 5V 的升压级**：ETA6098 只是把 VBUS 降压充进电池的开关充电器；
  电池正极经电源开关 Q5 直接搭在 VBUS 网络上（约 3.5–4.1V，低于 USB 手柄的枚举门限，纯电池点不亮手柄）。
- **5V 只能从外部注入**：VBUS 网络的注入点是 TP1（5V）、TP2（GND），注入同时经 ETA6098 给电池充电。
- **充电状态与外部供电没有可测网络**：ETA6098 的 STAT 引脚（9 脚）空置、没有引出任何网络，板上也没有 VBUS 检测网络；
  要拿到实测值只能另加测量——在 VBUS / PMID 网络上取分压接空闲 GPIO，或在电池回路串采样电阻并一颗电量计。
