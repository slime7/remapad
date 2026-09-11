# PocketJS 快照

本目录是 PocketJS 官方实现的固定快照，让 remapad 不依赖外部 checkout 即可完成 UI 检查、
编译、打包与触摸预览。内容来自 `pocketjs` 仓库：

| 项目 | 值 |
| :--- | :--- |
| 来源 | `pocket-stack/pocketjs` |
| 版本 | `@pocketjs/framework` 0.11.0 |
| 修订 | `b5e2a27447022c52e5065aabe2be728f464e5b86`（2026-09-10） |
| 许可 | MIT，见 `LICENSE` |

## 内容

```text
framework/     编译器、清单解析与框架运行时代码
contracts/     包格式、目标与 ESP-IDF host 契约
tools/         编译器入口（pocket.ts、build.ts 及其依赖）
assets/        构建期烘焙的字体与图片资源
hosts/web/     触摸预览使用的 wasm-ops.js 与官方 wasm 核心，以及官方 DevTools 服务器与面板
package.json   保留 name 与 exports，用于子路径解析
```

本目录只放上面这些内容：依赖（`vue`、`solid-js`、`@babel/*`、`opentype.js` 等）仍由
`ui/package.json` 安装，编译器需要的运行时依赖通过 `node_modules` 符号链接指向
`ui/node_modules`，由 `scripts/pocketjs.mjs` 在需要时自动建立。

## 为什么需要快照

npm 上发布的 `@pocketjs/framework` 0.11.0 不含 ESP-IDF host profile 支持（没有
`--host-profile` 参数，也没有 `framework/src/manifest/idf-host.ts` 与
`contracts/schema/pocket-idf-host-1.json`），而 ESP-IDF host 支持目前只存在于官方仓库。
把编译器快照进本仓库后，构建不再依赖机器上另一份 PocketJS 源码。

## 同步方式

登记上游更新时执行：

```powershell
node scripts/vendor-pocketjs.mjs <PocketJS checkout 路径>
```

随后重跑 `pnpm install`、`pnpm run build`，并用 `pnpm run dev` 确认触摸预览正常。若上游改动
了组件源码或 QuickJS 依赖，还要按 [patches/README.md](../../../patches/README.md) 重新对账
`firmware/components/` 与原生归档。

`framework/src/styles.generated.ts` 是编译期生成的文件，不纳入版本管理。
