// 屏幕界面 WASM 的构建脚本：编 wasm32 包、按 Cargo.lock 核对 wasm-bindgen-cli 版本、装配 ui/preview/dist。
// 用法：在 ui/preview 目录执行 node tools/build.mjs；首次运行自动补 wasm32 target 与 wasm-bindgen-cli（要等几分钟）。
import { spawnSync } from "node:child_process";
import { copyFileSync, mkdirSync } from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

const wasmDir = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const uiDir = path.resolve(wasmDir, "..");
const rootDir = path.resolve(uiDir, "..");
const distDir = path.join(wasmDir, "dist");
const target = "wasm32-unknown-unknown";
const crate = "remapad-ui-wasm";
const wasmFile = path.join(uiDir, "target", target, "release", "remapad_ui_wasm.wasm");

function run(command, args) {
  const result = spawnSync(command, args, { stdio: "inherit", cwd: rootDir });
  if (result.error) {
    throw result.error;
  }
  if (result.status !== 0) {
    throw new Error(`${command} 退出码 ${result.status}`);
  }
}

function capture(command, args) {
  // cargo metadata 的输出有好几 MB，spawnSync 默认 1MB 缓冲会被掐断。
  const result = spawnSync(command, args, { encoding: "utf8", cwd: rootDir, maxBuffer: 256 * 1024 * 1024 });
  if (result.error) {
    throw result.error;
  }
  if (result.status !== 0) {
    throw new Error(`${command} ${args.join(" ")} 失败：${result.stderr}`);
  }
  return result.stdout;
}

function ensureTarget() {
  const installed = capture("rustup", ["target", "list", "--installed"]);
  if (!installed.split(/\s+/).includes(target)) {
    run("rustup", ["target", "add", target]);
  }
}

/** 工作区解析到的 wasm-bindgen 版本；semver 兼容的多个取最大（wasm 里链接的是它）。 */
function lockedWasmBindgenVersion() {
  const metadata = JSON.parse(capture("cargo", ["metadata", "--format-version", "1", "--manifest-path", path.join(uiDir, "Cargo.toml")]));
  const versions = metadata.packages.filter((pkg) => pkg.name === "wasm-bindgen").map((pkg) => pkg.version);
  if (versions.length === 0) {
    throw new Error("依赖里没有 wasm-bindgen：先成功跑一次 build");
  }
  return versions.sort().at(-1);
}

function ensureWasmBindgenCli(version) {
  const probe = spawnSync("wasm-bindgen", ["--version"], { encoding: "utf8" });
  const current = probe.status === 0 ? probe.stdout.trim() : "";
  if (current.includes(version)) {
    return;
  }
  console.log(`[ui-wasm] wasm-bindgen-cli 缺失或版本不符（${current || "未安装"}），安装 ${version}，要等几分钟`);
  run("cargo", ["install", "wasm-bindgen-cli", "--version", version, "--locked"]);
}

try {
  ensureTarget();
  run("cargo", ["build", "--manifest-path", path.join(uiDir, "Cargo.toml"), "-p", crate, "--target", target, "--release"]);
  ensureWasmBindgenCli(lockedWasmBindgenVersion());
  mkdirSync(distDir, { recursive: true });
  run("wasm-bindgen", ["--target", "web", "--out-dir", distDir, "--out-name", "remapad_ui_wasm", wasmFile]);
  copyFileSync(path.join(wasmDir, "index.html"), path.join(distDir, "index.html"));
  console.log(`[ui-wasm] 产物就绪：${distDir}`);
} catch (error) {
  console.error(`[ui-wasm] 构建失败：${error.message}`);
  process.exit(1);
}
