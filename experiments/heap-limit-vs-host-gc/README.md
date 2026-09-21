# 实验：只配 heap_limit、去掉宿主压力 GC（对应上游 pocket-stack/pocketjs#429）

## 环境与固件

- 板卡：微雪 ESP32-S3-Touch-LCD-1.69（ESP32-S3R8，8 MB PSRAM，240 × 280 触摸屏）。
- 被测固件：本分支 `e398ace`（= master `901afb9` + 去掉 ADR 0036 的宿主压力 GC + 落地上游 #460 的
  `CONFIG_POCKETJS_GUEST_HEAP_LIMIT`，预算取 6291456 字节）。
- 对照固件：master `901afb9`（保留 PSRAM 压力触发 GC，堆上限在代码里写 7372 KiB）。
- 负载：PC 侧脚本按固定间隔注入 `key r` / `key l`（L1/R1 循环切页），先发 `ui on` 进手柄操控屏幕模式；
  设备常亮、无主机连接。日志里大量 `debug key inject` 行就是这些注入的回显。

## 结果

同一负载节奏（1.2 s 一次按键，24 min 与 13 min）下两种固件的行为：

| 运行 | 时长 | js_heap | PSRAM 余量 | allocs | 结果 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| 无宿主 GC（旧上限 7372 KiB） | 24.0 min | 3255 → 3568 kB（+13.0 kB/min） | 2,070,916 → 1,613,420 B（−18.6 kB/min） | 58,038 → 67,211 | 全程一次回收都没有 |
| 宿主 GC（master） | 13.0 min | 3056 → 3058 kB（+0.2 kB/min） | 2,397,936 → 2,306,216 B（−6.9 kB/min） | 55,270 → 55,275 | 无异常，垃圾被回收 |

加负载到 0.35 s 一次按键后，被测固件在连续切页 49 分钟时崩溃：

| 指标 | 起点（设备时钟 437 s） | 崩溃前最后一行（3,377 s） |
| :--- | :--- | :--- |
| js_heap | 3,396 kB | 4,690 kB（上限 6,144 kB，从未触顶、也从未自动回收） |
| PSRAM 余量 | 1,862,116 B | 96,120 B |
| 内部 RAM 余量 | 165,183 B | 43,695 B |
| allocs | 62,122 | 99,693 |

崩溃现场（设备时钟 3,432 s，见 `no-host-gc-6mib-churn.txt`）：

```
E (3432388) pocketjs_ui_core: Rust core panicked
abort() was called at PC 0x42011dab on core 1
Backtrace: 0x40387bb1:0x3c384980 ...
rst:0xc (RTC_SW_CPU_RST),boot:0x8 (SPI_FAST_FLASH_BOOT)
```

崩溃时 JS 堆只用了 6,144 kB 上限的 76%，没有 JS 侧 OOM；死的是原生侧：PSRAM 只剩 96 kB 后分配器
翻到内部 RAM（165,183 → 43,695 B），`pocketjs_ui_core` 的 Rust 分配器失败 panic → `abort()` → 重启。
自动 GC 的触发条件是新建对象时 `malloc_size` 越过阈值，而阈值被启动期的逐级上调抬到 4,690 kB 之上，
所以在此之前一次都没触发；池子没等到那一步就见底了。

## 文件

| 文件 | 内容 |
| :--- | :--- |
| `no-host-gc-6mib-churn.txt` | 被测固件 `e398ace` + 0.35 s 切页，49 分钟后 abort 重启的完整串口日志 |
| `host-gc-churn.txt` | master `901afb9` + 同样切页负载的对照日志（js_heap 稳定、无异常） |

其余测量留在 `agent-temp/`（空闲 10 分钟零分配、旧上限 24 分钟无 GC、启动日志、两版镜像与驱动脚本）。
