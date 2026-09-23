// 固件主机端单元测试入口：把与硬件无关的固件源码编译成开发机上的可执行文件并运行。
//
// 不走 ESP-IDF 的 Unity 上板跑：那套每次都要串口与实板，改一行代码也要等烧录；
// 这里要的是几秒钟内出结果的回归网，因此只挑不依赖 IDF 运行时的纯逻辑模块
// （NS2 编码与序列号、命令帧、像素加速回调、输入源合成），缺失的 IDF 头文件用
// firmware/test/support/stubs 下的最小替身补齐，替身只参与本次编译。
//
// 编译器按 CC、MSVC（vcvars64）、clang、gcc 的顺序探测，任何一个都行。
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readdirSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const SCRIPT_DIR = dirname(fileURLToPath(import.meta.url));
const PROJECT_ROOT = resolve(SCRIPT_DIR, '..');
const BUILD_DIR = resolve(PROJECT_ROOT, 'firmware/build/host-tests');
const OBJ_DIR = resolve(BUILD_DIR, 'obj');
const EXE_NAME = process.platform === 'win32' ? 'remapad-host-tests.exe' : 'remapad-host-tests';
const EXE_PATH = resolve(BUILD_DIR, EXE_NAME);

/** 被测固件源码：只列与硬件无关的模块。 */
const FIRMWARE_SOURCES = [
  'firmware/main/pad/pad_state.c',
  'firmware/main/pad/layout.c',
  'firmware/main/pad/layouts/xbox.c',
  'firmware/main/pad/layouts/xinput.c',
  'firmware/main/pad/layouts/ds3.c',
  'firmware/main/pad/layouts/ds4.c',
  'firmware/main/pad/layouts/ds5.c',
  'firmware/main/pad/layouts/ns.c',
  'firmware/main/pad/pad_device.c',
  'firmware/main/pad/ds_behavior.c',
  'firmware/main/pad/feedback.c',
  'firmware/main/input/input_frame.c',
  'firmware/main/ota/ota_proto.c',
  'firmware/main/amiibo/amiibo_proto.c',
  'firmware/main/target/ns2/ns2_report.c',
  'firmware/main/target/ns2/ns2_adv.c',
  'firmware/main/target/ns2/ns2_serial.c',
  'firmware/main/target/ns2/ns2_frames.c',
  'firmware/main/target/ns2/ns2_nfc.c',
  'firmware/main/target/ns2/ns2_identity.c',
  'firmware/main/target/ns2/ns2_upgrade.c',
  'firmware/main/target/ns2/ns2_output.c',
  'firmware/main/target/target.c',
  'firmware/main/target/ns2/ns2_target.c',
  'firmware/main/render_accel.c',
  'firmware/main/render_damage.c',
  'firmware/main/dp/dp_source.c',
  'firmware/main/dp/dp_ui.c',
  'firmware/main/dp/dp_capture.c',
  'firmware/main/drivers/battery_curve.c',
  'firmware/main/usb/usb_audio_parse.c',
  'firmware/main/usb/haptic_synth.c',
];

/** 测试自身的源码与替身。 */
const TEST_SOURCES = [
  'firmware/test/support/host_test.c',
  'firmware/test/suites.c',
  'firmware/test/support/stubs/app_config_stub.c',
  'firmware/test/support/stubs/heap_caps_stub.c',
  'firmware/test/support/stubs/esp_timer_stub.c',
  'firmware/test/test_ns2_report.c',
  'firmware/test/test_ns2_adv.c',
  'firmware/test/test_ns2_serial.c',
  'firmware/test/test_ns2_frames.c',
  'firmware/test/test_ns2_upgrade.c',
  'firmware/test/test_ns2_identity.c',
  'firmware/test/test_render_accel.c',
  'firmware/test/test_render_damage.c',
  'firmware/test/test_dp_source.c',
  'firmware/test/test_dp_ui.c',
  'firmware/test/test_dp_capture.c',
  'firmware/test/test_pad_device.c',
  'firmware/test/test_ds_behavior.c',
  'firmware/test/test_pad_ns.c',
  'firmware/test/test_pad_feedback.c',
  'firmware/test/test_ns2_relay.c',
  'firmware/test/test_input_frame.c',
  'firmware/test/test_ota_proto.c',
  'firmware/test/test_ns2_nfc.c',
  'firmware/test/test_amiibo_proto.c',
  'firmware/test/test_target_ns2.c',
  'firmware/test/test_battery.c',
  'firmware/test/test_usb_audio.c',
  'firmware/test/test_haptic_synth.c',
];

const INCLUDE_DIRS = [
  'firmware/main',
  'firmware/main/pad',
  'firmware/main/input',
  'firmware/main/ota',
  'firmware/main/amiibo',
  'firmware/main/target',
  'firmware/main/target/ns2',
  'firmware/main/config',
  'firmware/main/dp',
  'firmware/main/drivers',
  'firmware/main/usb',
  'firmware/components/pocketjs_render_rgb565/include',
  'firmware/test/support',
  'firmware/test/support/stubs',
];

const sources = [...FIRMWARE_SOURCES, ...TEST_SOURCES].map((item) => resolve(PROJECT_ROOT, item));
const includes = INCLUDE_DIRS.map((item) => resolve(PROJECT_ROOT, item));

for (const file of sources) {
  if (!existsSync(file)) {
    console.error('[固件测试] 缺少源文件: ' + file);
    process.exit(1);
  }
}

function findVcvars() {
  const vswhere = 'C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe';
  if (existsSync(vswhere)) {
    const probe = spawnSync(vswhere, ['-products', '*', '-property', 'installationPath'], {
      encoding: 'utf8',
    });
    for (const line of String(probe.stdout ?? '').split(/\r?\n/)) {
      const root = line.trim();
      if (root === '') continue;
      const candidate = join(root, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat');
      if (existsSync(candidate)) return candidate;
    }
  }
  for (const base of [
    'C:\\Program Files\\Microsoft Visual Studio',
    'C:\\Program Files (x86)\\Microsoft Visual Studio',
  ]) {
    if (!existsSync(base)) continue;
    for (const version of readdirSync(base)) {
      const editions = join(base, version);
      let names = [];
      try {
        names = readdirSync(editions);
      } catch {
        continue;
      }
      for (const edition of names) {
        const candidate = join(editions, edition, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat');
        if (existsSync(candidate)) return candidate;
      }
    }
  }
  return null;
}

/** 用 MSVC 编译：vcvars64 先配环境，再用响应文件传参数（省去 cmd 的引号地狱）。 */
function buildWithMsvc(vcvars) {
  // 路径统一写成正斜杠：MSVC 两种都收，而正斜杠不会和参数收尾引号打架
  // （`/Fo"C:\dir\"` 里的反斜杠会把引号转义掉）。
  const slashes = (value) => value.replace(/\\/g, '/');
  const response = [
    '/nologo',
    '/utf-8',
    '/std:c11',
    '/W4',
    '/Fo"' + slashes(OBJ_DIR) + '/"',
    '/Fe:"' + slashes(EXE_PATH) + '"',
    ...includes.map((dir) => '/I"' + slashes(dir) + '"'),
    ...sources.map((file) => '"' + slashes(file) + '"'),
  ].join('\r\n');
  const responsePath = join(BUILD_DIR, 'compile.rsp');
  writeFileSync(responsePath, response + '\r\n', 'utf8');

  const script = [
    '@echo off',
    'call "' + vcvars + '" >nul',
    'if errorlevel 1 exit /b 1',
    'cl @"' + responsePath + '"',
    'exit /b %errorlevel%',
  ].join('\r\n');
  const scriptPath = join(BUILD_DIR, 'build-host-tests.bat');
  writeFileSync(scriptPath, script + '\r\n', 'utf8');

  const result = spawnSync('cmd.exe', ['/c', scriptPath], { stdio: 'inherit' });
  return result.status === 0;
}

function buildWithPosixCompiler(compiler) {
  const args = [
    '-std=c11',
    '-Wall',
    '-Wextra',
    '-Wno-unused-parameter',
    ...includes.map((dir) => '-I' + dir),
    '-o',
    EXE_PATH,
    ...sources,
  ];
  const result = spawnSync(compiler, args, { stdio: 'inherit' });
  return result.status === 0;
}

function pickCompiler() {
  if (process.env.CC && process.env.CC.trim() !== '') {
    const compiler = process.env.CC.trim();
    return { name: compiler, build: () => buildWithPosixCompiler(compiler) };
  }
  if (process.platform === 'win32') {
    const vcvars = findVcvars();
    if (vcvars !== null) {
      return { name: 'MSVC (' + vcvars + ')', build: () => buildWithMsvc(vcvars) };
    }
  }
  for (const compiler of ['clang', 'gcc', 'cc']) {
    const probe = spawnSync(compiler, ['--version'], { stdio: 'ignore' });
    if (probe.status === 0) {
      return { name: compiler, build: () => buildWithPosixCompiler(compiler) };
    }
  }
  return null;
}

const compiler = pickCompiler();
if (compiler === null) {
  console.error('[固件测试] 找不到 C 编译器。');
  console.error('[固件测试] Windows 上装 Visual Studio Build Tools 即可；其他平台装 clang 或 gcc。');
  console.error('[固件测试] 也可以直接指定：$env:CC = "clang"');
  process.exit(1);
}

rmSync(OBJ_DIR, { recursive: true, force: true });
mkdirSync(OBJ_DIR, { recursive: true });
console.log('[固件测试] 编译器: ' + compiler.name);
if (!compiler.build()) {
  console.error('[固件测试] 编译失败');
  process.exit(1);
}

const run = spawnSync(EXE_PATH, [], { stdio: 'inherit' });
process.exit(run.status ?? 1);
