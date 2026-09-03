import { spawnSync } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const SCRIPT_DIR = dirname(fileURLToPath(import.meta.url));
const UI_ROOT = resolve(SCRIPT_DIR, '..');
const INSTALLED_FRAMEWORK_ROOT = resolve(UI_ROOT, 'node_modules/@pocketjs/framework');
const CONFIGURED_FRAMEWORK_ROOT = process.env.POCKETJS_ROOT?.trim()
  ? resolve(process.env.POCKETJS_ROOT.trim())
  : null;
const command = process.argv[2];

if (!['check', 'compile', 'build'].includes(command)) {
  console.error('usage: node scripts/pocket.mjs <check|compile|build>');
  process.exit(1);
}

function hasHostProfileCompiler(root) {
  const script = resolve(root, 'tools/pocket.ts');
  try {
    return existsSync(script) && readFileSync(script, 'utf8').includes('--host-profile');
  } catch {
    return false;
  }
}

const frameworkCandidates = [CONFIGURED_FRAMEWORK_ROOT, INSTALLED_FRAMEWORK_ROOT]
  .filter((root, index, roots) => root !== null && roots.indexOf(root) === index);
const FRAMEWORK_ROOT = frameworkCandidates.find(hasHostProfileCompiler);

if (!FRAMEWORK_ROOT) {
  console.error(
    '[Remapad] PocketJS host-profile compiler not found. Set POCKETJS_ROOT to an updated PocketJS checkout.',
  );
  process.exit(1);
}

const POCKET_SCRIPT = resolve(FRAMEWORK_ROOT, 'tools/pocket.ts');

const args = [
  command,
  '--host-profile',
  resolve(UI_ROOT, '../firmware/pocket.host.json'),
  '--manifest',
  resolve(UI_ROOT, 'pocket.json'),
  '--project-root',
  UI_ROOT,
  '--outdir',
  resolve(UI_ROOT, 'dist'),
];

if (command === 'build') {
  args.push('--output', resolve(UI_ROOT, 'dist/remapad-ui.pocket'));
}

const separator = process.argv.indexOf('--');
if (separator >= 0) {
  args.push('--', ...process.argv.slice(separator + 1));
}

const result = spawnSync('bun', [POCKET_SCRIPT, ...args], {
  cwd: FRAMEWORK_ROOT,
  stdio: 'inherit',
});

if (result.error) {
  console.error('[Remapad] PocketJS CLI failed to start:', result.error.message);
  process.exit(1);
}

process.exit(result.status ?? 1);
