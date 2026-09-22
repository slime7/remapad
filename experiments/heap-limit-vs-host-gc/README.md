# 实验：只配 heap_limit、收回宿主压力 GC（对应上游 pocket-stack/pocketjs#429）

## 代码状态

- 固件组件对账至上游 `main`（commit `ea25791da4ef75d95467cf83c230241114d76c70`，含 PR #460）：仓库副本与上游只余
  [patches/README.md](../../patches/README.md) 记录的 0001 / 0003 / 0004 三处差异，构建与 #460 之后的官方代码一致。
- 本分支按上游回复的设想移除 ADR 0036 的宿主压力 GC：宿主不再主动 `JS_RunGC`，JS 堆回收只交给引擎。
  这处移除只为本轮取证；产品路径仍按 ADR 0036 在宿主侧按 PSRAM 压力回收，本分支的固件改动不直接合并。
- 预算走 `CONFIG_POCKETJS_GUEST_HEAP_LIMIT`（`firmware/sdkconfig`），本轮取上游默认 4 MiB 与产品取值 6 MiB 两档。
- 60 秒一行的 `mem:` 日志新增 `gc_thresh=`（读 `JS_GetGCThreshold`），可以直接对照引擎阈值式 GC 的阈值与堆水位。
- 板卡：微雪 ESP32-S3-Touch-LCD-1.69（ESP32-S3R8，8 MB PSRAM，240 × 280 触摸屏），设备常亮、无主机连接。
- 负载：`ui on` 进手柄操控屏幕模式后，每 0.35 秒交替注入 `key r` / `key l` 切页。

## 结论

设置 `config.heap_limit` 只换掉 `JS_SetMemoryLimit` 的预算数值，不产生 GC：预算与阈值式 GC 的阈值是两条互不相通的路径。
`js_malloc_rt` 越过 `malloc_limit` 直接返回 NULL；`js_trigger_gc` 只比较 `malloc_size` 与 `malloc_gc_threshold`，
后者每次触发后抬到存活堆的 1.5 倍，与预算无关。
实测两档预算都只走到「分配失败」或「物理内存先见底」，全程一次回收都没有：

| 预算 | 负载时长 | 实测 GC 阈值 | 采样点 | 结果 |
| :--- | :--- | :--- | :--- | :--- |
| 4 MiB（上游默认） | 23 分钟 | 5704 kB | 24 个，堆零回落 | 堆钉在 4095/4096 kB，计数全线冻结，帧错误 376 次（`[error] null`），设备存活但界面失效 |
| 6 MiB（产品取值） | 35 分钟 | 5704 kB | 35 个，堆零回落 | 堆 3265 → 4699 kB，PSRAM 剩 96.5 kB、内部 RAM 剩 40 kB 时 `pocketjs_ui_core` panic → `abort()` → 重启 |
| 6 MiB + 宿主压力 GC（先前对照） | 13 分钟 | — | — | js_heap 稳定 3056 kB，PSRAM 基本不降 |

阈值 5704 kB 来自启动期最后一次阈值回收（阈值 = 存活堆 × 1.5，当时存活堆约 3.8 MB：7 个常驻页面 + devtools 飞行记录仪），
而稳态存活堆只有约 3.2–3.4 MB。下一轮阈值回收要再等堆涨 1–2 MB：4 MiB 预算下阈值比预算本身高 1.6 MB，
6 MiB 预算下阈值虽在预算内，但物理 PSRAM 先耗尽。两档预算的启动日志里 `js heap after eval: used=3244kB` 完全相同，
差别只在预算大小，可见 #460 只是把上限换了个数字，GC 调度没有变化。

## 4 MiB 预算：撞上限只得到分配失败

| 指标 | 起点（设备时钟 137 s） | 撞上限（1 337 s） | 冻结段（1 397–1 637 s） |
| :--- | :--- | :--- | :--- |
| js_heap | 3349 kB / 4096 kB | 4094 kB | 4095 kB（逐行不变） |
| gc_thresh | 5704 kB | 5704 kB | 5704 kB（从未变化） |
| PSRAM 余量 | 1,913,032 B | 807,148 B | 805,156 B（不再下降） |
| allocs / obj / prop | 60,618 / 16,697 / 54,610 | 82,421 / 22,660 / 63,102 | 82,488 / 22,667 / 63,003（冻结） |
| 帧错误 | 0 | — | 376（`mem` 应答：`turns=23759 errors=376`） |

堆撞上 4096 kB 的瞬间开始，每帧都打一行 `[error] null`；此后 `allocs`、`obj_n`、`prop_n` 与 PSRAM 余量逐行相同：
JS 侧分配全部失败，环垃圾不再增加也回收不掉，界面停在失败前的位置。app 与 devtools 都还在跑，只是再也分配不到内存。

## 6 MiB 预算：物理内存先见底，原生侧 abort

| 指标 | 起点（设备时钟 437 s） | 崩溃前最后一行（2 177 s） |
| :--- | :--- | :--- |
| js_heap | 3265 kB / 6144 kB | 4699 kB（阈值 5704 kB 还差 1 MB，预算还差 1.4 MB） |
| PSRAM 余量 | 2,034,864 B | 98,824 B |
| 内部 RAM 余量 | 165,199 B | 40,999 B |
| allocs / obj / prop | 58,189 / 16,031 / 53,648 | 99,848 / 27,556 / 70,941 |

```
[2061.97s] I (2176972) remapad_pocketjs: mem: js_heap=4699kB/6144kB gc_thresh=5704kB psram_free=98824 ...
[2090.43s] E (2205431) pocketjs_ui_core: Rust core panicked
[2090.43s] abort() was called at PC 0x42011d8f on core 1
[2090.43s] Backtrace: 0x40387bb1:0x3c384460 ...
[2090.43s] rst:0xc (RTC_SW_CPU_RST),boot:0x9 (SPI_FAST_FLASH_BOOT)
```

死因与 issue 里报的一致：JS 堆没触顶（上限 6144 kB、阈值 5704 kB 都没到），PSRAM 先被共享它的原生分配吃光，
`pocketjs_ui_core` 的 Rust 分配器失败 panic → `abort()` → 重启。宿主不回收时，环垃圾在两个阈值 GC 之间的增量
（1–2 MB）比 8 MB PSRAM 留给原生侧的余量还大。

## 与先前对照的差异

- 先前两份日志（`no-host-gc-6mib-churn.txt`、`host-gc-churn.txt`）出自 #460 手工落地的分支 `e398ace`；
  #460 进入上游后组件源码逐字节相同（差异只有 `guest_interrupt` 让出与 quickjs 哈希两条既有补丁），
  本轮日志来自官方快照，结论不变。
- 本轮新增 `gc_thresh=` 列，把「阈值被启动期存活堆钉在 5.7 MB」从推断变成实测读数。

## 文件

| 文件 | 内容 |
| :--- | :--- |
| `limit-4mib-boot.txt` | 4 MiB 预算启动日志（`used=3244kB limit=4096kB`，空闲读数 `gc_thresh=5704kB`） |
| `limit-4mib-churn.txt` | 4 MiB + 0.35 s 切页：全部 `remapad_pocketjs` 行与 `[error] null` 行（无切页噪音） |
| `limit-6mib-boot.txt` | 6 MiB 预算启动日志 |
| `limit-6mib-churn.txt` | 6 MiB + 0.35 s 切页：同上，到 abort 重启为止 |
| `no-host-gc-6mib-churn.txt` | 先前对照：`e398ace` + 上限 7372 KiB + 0.35 s 切页，49 分钟后 abort 的完整日志 |
| `host-gc-churn.txt` | 先前对照：master `901afb9`（保留宿主压力 GC）+ 同样切页负载的完整日志 |

未裁剪的原始串口日志、两版镜像与驱动脚本留在 `agent-temp/`（含 `churn-4mib.log`、`churn-6mib.log`）。
