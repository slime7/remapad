# 上游对账记录

本目录记录本仓库固定的 PocketJS 组件与上游实现之间的差异，以及重新对账的方法。
当前固件组件对账至 PocketJS v0.12.0（commit `d3f0be0c7739ea704ca25d8b5158581bb176abc6`）。
这些差异都已经写进 `firmware/components/` 内的仓库副本，构建时不需要再对任何目录打补丁。

## 0001-quickjs-ng-0.14.0-source-pin

`pocketjs_guest` 在编译前用 `prepare_quickjs.py` 校验 QuickJS 源码哈希，并且只接受被校验过的
源码。上游记录的哈希与 ESP Component Registry 实际提供的 `espressif/quickjs-ng` 0.14.0 不一致，
会导致 `idf.py build` 在 configure 阶段直接失败。

Registry 的 `espressif__quickjs-ng-v0.14.0.zip` 内 `quickjs.c` 的实际哈希是
`36128da188cb236ffd029dd3c672ff8f85e5a196a9211e267a515c8efc1ab52c`（Registry 只有 0.14.0 一个版本，副本经过重新打包）。

本仓库的处理方式是把 `firmware/components/pocketjs_guest/tools/prepare_quickjs.py` 中的
`SOURCE_SHA256` 直接改成 Registry 实际内容。`0001-quickjs-ng-0.14.0-source-pin.patch` 是这份差异的
记录，用来说明仓库副本相对上游改了什么，不需要对 PocketJS checkout 执行 `git apply`。

## 0002-ui-core-native-archive-in-tree

上游 `pocketjs_ui_core` 组件自带 `.gitignore` 忽略整个 `lib/`：原生归档在 CI 里构建，不进源码树。
本仓库的约定相反，ESP32-S3 归档随组件提交，克隆后不需要 Rust。沿用上游规则会让
`firmware/components/pocketjs_ui_core/lib/esp32s3/` 下的归档与 build receipt 进不了 Git，克隆出来的
仓库在 configure 阶段报 `Missing pocketjs_idf_ui_core for esp32s3`。仓库副本因此平掉了这条忽略规则；
`pocketjs_render_rgb565` 本来就没有它，两者现在一致。

## 0003-ui-qjs-touch-hit-facts-capture-table

上游 `pocketjs_ui_qjs` 的 `pocketjs_ui_turn` 用 native `pocketjs_ui_core_touch_hits`
解析触摸命中事实，且只在 `touch_count != 0` 时调用。设备上表现为两个层面的故障：

1. 跳过空帧使核心内的命中捕获表（归档符号 `pocketjs_core::touch::HitTable`）永不清理
   已抬起的触点 id——CST816T 单点触点 id 恒为 0，开机后第一次按压解析出的节点被永久
   携带，之后任意位置的触摸都命中该节点（表现为"全屏触摸都在按同一个按钮"）。
2. 改为每帧调用（含空帧，即官方宿主契约要求的节奏）后，空帧调用之后的 resolve 不再
   返回有效节点 id，任何触摸都找不到目标（表现为"按钮完全无响应"）。native 归档无源码
   可查，无法进一步定位。

本仓库的处理方式是不再调用 native `pocketjs_ui_core_touch_hits`，改为在
`firmware/components/pocketjs_ui_qjs/src/ui_qjs.c` 内用 C 移植框架官方的宿主侧参考
实现（`ui/vendor/pocketjs/framework/src/touch.ts` 的 `createTouchHitFacts`，注释标注
"Rust twin: pocketjs_core::Ui::touch_hits"）：新触点 id 经
`pocketjs_ui_core_hit_test_bounds`（规范 op 42，对当前布局树的通用几何查询）解析一次，
触点存续期间携带，抬起后由空帧清除。该行为已用固件注入合成点击端到端验证
（按钮坐标 → 命中按钮节点 → `onPress` 触发 → 串口输出；空白坐标正确不触发）。

`0003-ui-qjs-touch-hit-facts-capture-table.patch` 是这份差异的记录，用于升级组件时
重新对账，不需要执行 `git apply`。升级组件时应优先确认上游是否已修复
`pocketjs_ui_core_touch_hits` 的空帧行为，修复后可回退本补丁。

## 0004-guest-interrupt-handler-periodic-yield

上游 `pocketjs_guest` 的 `guest_interrupt` 只负责消费 `interrupt_epoch` 终止请求，
正常执行路径对调度零让出。设备上的表现：初始化 bundle 的首次 `JS_Eval` 是一段
约 11 秒的连续解释器执行，期间 `remapad-pjs`（优先级 5）一直占据 CPU 0，IDLE0
饿死，触发默认订阅空闲任务的 task watchdog（超时 5s）在 6.7s 与 11.7s 各打印
一次；稳态帧循环因帧间等待走信号量阻塞不受影响。

本仓库的处理方式是在 `firmware/components/pocketjs_guest/src/guest.c` 的
`guest_interrupt` 内加时间门控让出：解释器每约 1 万条指令轮询一次该 handler，
门控（20ms）到期时执行一次 `vTaskDelay(1)`，IDLE0 借块运行喂狗；短于门控的
turn 不会延迟，`pocketjs_guest_interrupt` 的终止语义原样保留。CMakeLists 相应
补上 `esp_timer`、`freertos` 私有依赖。

`0004-guest-interrupt-handler-periodic-yield.patch` 是这份差异的记录，用于升级
组件时重新对账，不需要执行 `git apply`。升级组件时应确认上游是否已在长 eval
场景处理空闲任务喂狗，处理后可回退本补丁。

## 0005-guest-heap-limit-kconfig

上游 PR `pocket-stack/pocketjs#460`（对应 issue #429）把 guest 的 QuickJS 堆上限做成组件 Kconfig：
新增 `CONFIG_POCKETJS_GUEST_HEAP_LIMIT`（默认 4194304 字节），`pocketjs_guest_config_defaults()` 改为读取它，
显式赋值 `config.heap_limit` 仍可覆盖。它只改预算来源与默认值，不改变 GC 调度。

本仓库副本已按该 PR 落地：`firmware/components/pocketjs_guest/Kconfig` 与 `src/guest.c` 的默认值取值方式
一致；产品固件不再在 `pocketjs_host.c` 里写死上限，取值回到 `firmware/sdkconfig.defaults`。
升级组件到含该 PR 的版本后，本条目只剩「预算取值写在 sdkconfig.defaults」这一条本仓库约定。

## 重新对账的方法

升级 `firmware/components/` 中的组件、或 Registry 的 `espressif/quickjs-ng` 内容发生变化时：

1. 用上游 `hosts/esp-idf/components/` 覆盖仓库副本。
2. 构建一次固件；如果 `pocketjs_guest` 报
   `unsupported QuickJS source; review immutable-buffer patch before upgrading`，说明源码标识变了。
3. 取出 Registry 实际内容并核对：

   ```powershell
   $zip = "$env:TEMP/quickjs-ng-0.14.0.zip"
   $base = 'https://components-file.espressif.com/components/espressif/quickjs-ng/0.14.0'
   Invoke-WebRequest "$base/espressif__quickjs-ng-v0.14.0.zip" -OutFile $zip
   $code = 'import zipfile,sys,hashlib; z=zipfile.ZipFile(sys.argv[1]); ' +
     '[print(n, hashlib.sha256(z.read(n)).hexdigest()) for n in z.namelist() if n.endswith(''quickjs.c'')]'
   python -c $code $zip
   ```

4. 确认不可变 ArrayBuffer 补丁仍然适用：把源码哈希临时替换为实际值后运行 `prepare_quickjs.py` 的
   `prepare()`，能正常产出即为适用；随后把实际值写入仓库副本，并更新本文件与补丁记录。
5. 用固定版本的 Xtensa Rust 重新生成原生归档（`pnpm run native`），并确认
   `firmware/components/*/lib/esp32s3/build-receipt.json` 中的编译器信息与上游 `toolchains.json` 一致。

## 不可变 ArrayBuffer 补丁为什么必须保留

PocketJS 把 PAK 以借用字节的形式交给 JavaScript，并把对外的 ArrayBuffer 视为不可修改。
QuickJS 0.14.0 的 `TypedArray.prototype.reverse` 与 species 构造路径缺少不可写检查，绕过检查就会
改写固件借用的 PAK 区域。`prepare_quickjs.py` 在构建目录的副本上补上这些检查，从不修改
`managed_components/` 中的原始文件。
