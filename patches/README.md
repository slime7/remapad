# 上游对账记录

本目录记录本仓库固定的 PocketJS 组件与上游实现之间的差异，以及重新对账的方法。
这些差异都已经写进 `firmware/components/` 内的仓库副本，构建时不需要再对任何目录打补丁。

## 0001-quickjs-ng-0.14.0-source-pin

`pocketjs_guest` 在编译前用 `prepare_quickjs.py` 校验 QuickJS 源码哈希，并且只接受被校验过的
源码。上游记录的哈希与 ESP Component Registry 实际提供的 `espressif/quickjs-ng` 0.14.0 不一致，
会导致 `idf.py build` 在 configure 阶段直接失败。

核对过的事实：

- Registry 的 `CHECKSUMS.json` 与下载得到的 `espressif__quickjs-ng-v0.14.0.zip` 都给出
  `quickjs-ng/quickjs.c` = `36128da188cb236ffd029dd3c672ff8f85e5a196a9211e267a515c8efc1ab52c`。
- Registry 只发布了 0.14.0 一个版本，没有其它版本可以满足上游记录的哈希。
- 上游 quickjs-ng v0.14.0 的 `quickjs.c` 又是另一个哈希，说明 Registry 的副本经过重新打包。
- `prepare_quickjs.py` 的全部结构断言（`js_typed_array_reverse` 中 `if (len > 0) {` 的唯一性、
  两处 `js_typed_array___speciesCreate` 声明、`2, args` 与 `4, args` 调用点）在 Registry 当前源码上
  仍然成立，不可变 ArrayBuffer 补丁本身依然适用。

本仓库的处理方式是把 `firmware/components/pocketjs_guest/tools/prepare_quickjs.py` 中的
`SOURCE_SHA256` 直接改成 Registry 实际内容。`0001-quickjs-ng-0.14.0-source-pin.patch` 是这份差异的
记录，用来说明仓库副本相对上游改了什么，不需要对 PocketJS checkout 执行 `git apply`。

## 0002-ui-core-native-archive-in-tree

上游 `pocketjs_ui_core` 组件自带 `.gitignore` 忽略整个 `lib/`：原生归档在 CI 里构建，不进源码树。
本仓库的约定相反，ESP32-S3 归档随组件提交，克隆后不需要 Rust。沿用上游规则会让
`firmware/components/pocketjs_ui_core/lib/esp32s3/` 下的归档与 build receipt 进不了 Git，克隆出来的
仓库在 configure 阶段报 `Missing pocketjs_idf_ui_core for esp32s3`。仓库副本因此平掉了这条忽略规则；
`pocketjs_render_rgb565` 本来就没有它，两者现在一致。

## 重新对账的方法

升级 `firmware/components/` 中的组件、或 Registry 的 `espressif/quickjs-ng` 内容发生变化时：

1. 用上游 `hosts/esp-idf/components/` 覆盖仓库副本。
2. 构建一次固件；如果 `pocketjs_guest` 报
   `unsupported QuickJS source; review immutable-buffer patch before upgrading`，说明源码标识变了。
3. 取出 Registry 实际内容并核对：

   ```powershell
   $zip = "$env:TEMP/quickjs-ng-0.14.0.zip"
   Invoke-WebRequest 'https://components-file.espressif.com/components/espressif/quickjs-ng/0.14.0/espressif__quickjs-ng-v0.14.0.zip' -OutFile $zip
   python -c "import zipfile,sys,hashlib; z=zipfile.ZipFile(sys.argv[1]); [print(n, hashlib.sha256(z.read(n)).hexdigest()) for n in z.namelist() if n.endswith('quickjs.c')]" $zip
   ```

4. 确认不可变 ArrayBuffer 补丁仍然适用：把源码哈希临时替换为实测值后运行 `prepare_quickjs.py` 的
   `prepare()`，能正常产出即为适用；随后把实测值写入仓库副本，并更新本文件与补丁记录。
5. 用固定版本的 Xtensa Rust 重新生成原生归档（`pnpm run native`），并确认
   `firmware/components/*/lib/esp32s3/build-receipt.json` 中的编译器信息与上游 `toolchains.json` 一致。

## 不可变 ArrayBuffer 补丁为什么必须保留

PocketJS 把 PAK 以借用字节的形式交给 JavaScript，并把对外的 ArrayBuffer 视为不可修改。
QuickJS 0.14.0 的 `TypedArray.prototype.reverse` 与 species 构造路径缺少不可写检查，绕过检查就会
改写固件借用的 PAK 区域。`prepare_quickjs.py` 在构建目录的副本上补上这些检查，从不修改
`managed_components/` 中的原始文件。
