# 依赖补丁

本目录保存必须应用到 PocketJS checkout 的补丁。补丁不修改 PocketJS 的行为或格式，
只修正无法从 ESP Component Registry 获取的内容标识。

应用方式（`POCKETJS_ROOT` 指向本地 PocketJS checkout）：

```powershell
git -C $env:POCKETJS_ROOT apply F:/private/remapad/patches/0001-quickjs-ng-0.14.0-source-pin.patch
```

检查补丁是否已经应用：

```powershell
git -C $env:POCKETJS_ROOT apply --check --reverse F:/private/remapad/patches/0001-quickjs-ng-0.14.0-source-pin.patch
```

## 0001-quickjs-ng-0.14.0-source-pin

`pocketjs_guest` 在编译前用 `prepare_quickjs.py` 校验 QuickJS 源码哈希，并拒绝写入
不可变 ArrayBuffer 的补丁以外的源码。该脚本记录的哈希与 ESP Component Registry 当前
提供的 `espressif/quickjs-ng` 0.14.0 源码不一致，导致 `idf.py build` 在 configure 阶段
直接失败。

事实核对结果：

- Registry 的 `CHECKSUMS.json` 与下载的 `espressif__quickjs-ng-v0.14.0.zip` 都给出
  `quickjs-ng/quickjs.c` = `36128da188cb236ffd029dd3c672ff8f85e5a196a9211e267a515c8efc1ab52c`。
- registry 只发布了 0.14.0 一个版本，没有其他可用版本可以满足原哈希。
- 上游 quickjs-ng v0.14.0 的 `quickjs.c` 是另一个哈希，说明该文件曾按平台重新打包。
- `prepare_quickjs.py` 的全部结构断言（`js_typed_array_reverse` 的 `if (len > 0) {` 唯一性、
  两处 `js_typed_array___speciesCreate` 声明、`2, args` 与 `4, args` 调用点）在 Registry 当前源码上
  均通过，补丁仍然适用。
- 本仓库旧版 `firmware/components/pocketjs_guest` 副本中已经使用同一个 `36128da1…` 取值。

因此补丁只把 `SOURCE_SHA256` 更新为 Registry 实际提供的内容。PocketJS 更新该常量或换用
新版本 `espressif/quickjs-ng` 后，应先移除本补丁再重试。
