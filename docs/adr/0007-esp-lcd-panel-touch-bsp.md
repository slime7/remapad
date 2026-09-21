# 0007 — 显示与触摸 BSP 采用 esp_lcd 内置驱动与 Registry 触摸组件

- 状态: active
- 日期: 2026-09-11
- 替代: 无

## 背景

板卡集成 ST7789V2（4-wire SPI，只写 DIN，BL=GPIO15 需显式驱动）与 CST816T（I2C 0x15，与 IMU/RTC 共享 GPIO10/11 总线）。官方 PocketJS host 只产出 RGB565 damage strip 和空输入契约，面板传输、触摸采样与背光属于产品固件 BSP 职责；时序与方向配置需要一块真实 240x280 面板的既成参考，微雪官方 ESP-IDF 示例（02_ESP_IDF_ST7789_LVGL）已验证 mirror(true,true)+invert+gap(0,20) 组合。

## 决策

面板使用 ESP-IDF 内置 esp_lcd_new_panel_st7789（SPI2 40 MHz，psram_dma_direct 直读 PSRAM strip），初始化参数与微雪示例一致；传输前对 RGB565 像素原地做大小端字节交换以匹配 SPI 线序。触摸使用 Registry 组件 espressif/esp_lcd_touch_cst816s（依赖 espressif/esp_lcd_touch），经 i2c_master 驱动挂 0x15，并打开 DISABLE_READ_ID 规避 T 变体芯片 ID 读取失败。背光用 GPIO15 上的 LEDC PWM（25 kHz）。strip 在渲染事务内经 panel_transfer 提交，传输失败走 abort 丢弃整帧。

## 考虑的方案

- 自写寄存器级 ST7789/CST816T BSP：零外部依赖，但需自行维护初始化序列、DMA 描述符与缓存一致性，长期成本高
- 引入 esp_lvgl_port/LVGL 显示栈：自带 damage 缓冲与触摸集成，但项目渲染核心是 PocketJS，LVGL 栈纯属冗余
- 采用 esp_lcd 内置 ST7789 驱动 + Registry CST816S 组件（已选）：DMA/缓存细节由官方维护，时序参数可逐条对照微雪示例

## 影响

- 驱动代码收敛到 panel / touch / backlight 三个薄模块，PSRAM strip 可被 EDMA 直读。
- 代价：main 组件新增两个 Registry 组件依赖；每帧传输前的一次全 strip 字节交换计入帧预算；
  触摸驱动私有持有 I2C master bus，后续接入 IMU/RTC 时必须重构出共享总线持有者。
