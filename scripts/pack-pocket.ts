import { existsSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { canonicalJson } from '../ui/node_modules/@pocketjs/framework/framework/src/manifest/plan.ts';
import { encodePocketPackage } from '../ui/node_modules/@pocketjs/framework/contracts/spec/pocket-package.ts';
import { makeVariant } from '../ui/node_modules/@pocketjs/framework/tools/pocket-pack.ts';

const [planPath, manifestPath, outdir, outputPath] = process.argv.slice(2);
const plan = JSON.parse(readFileSync(planPath, 'utf8'));
const js = new Uint8Array(readFileSync(resolve(outdir, `${plan.app.output}.js`)));
const pakPath = resolve(outdir, `${plan.app.output}.pak`);
const pak = existsSync(pakPath) ? new Uint8Array(readFileSync(pakPath)) : new Uint8Array(0);
const variant = makeVariant({
  target: plan.target.id,
  hostAbi: plan.target.hostAbi,
  planJson: canonicalJson(plan),
  identity: { output: plan.app.output, id: plan.app.id, title: plan.app.title },
  js,
  pak,
});
await Bun.write(outputPath, encodePocketPackage({
  manifest: new Uint8Array(readFileSync(manifestPath)),
  variants: [variant],
}));
console.log(`✓ ESP-IDF package ${outputPath}`);
