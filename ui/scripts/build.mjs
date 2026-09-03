import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';
import { transformSync } from '@babel/core';
import { transformVueJsxVapor } from 'vue-jsx-vapor/api';
import {
  propsHelperCode,
  propsHelperId,
  vaporHelperCode,
  vaporHelperId,
  vdomHelperCode,
  vdomHelperId,
} from '@vue-jsx-vapor/runtime/raw';
import { compileClasses, generateStylesModule } from '../node_modules/@pocketjs/framework/framework/compiler/tailwind.ts';
import { bakeAtlases } from '../node_modules/@pocketjs/framework/framework/compiler/bake-font.ts';
import { decodePng, encodeImageEntry, keyImage, PAK_DTYPE, pack, KEY_STYLES } from '../node_modules/@pocketjs/framework/framework/compiler/pak.ts';
import { bakeSvg } from '../node_modules/@pocketjs/framework/framework/compiler/bake-svg.ts';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const ROOT = path.resolve(__dirname, '..');
const DIST = path.join(ROOT, 'dist');
const FW_MAIN = path.resolve(ROOT, '../firmware/main');

const POCKET_PACKAGE_MAGIC = 0x544b4350;
const POCKET_PACKAGE_VERSION = 1;
const POCKET_PACKAGE_HEADER_SIZE = 16;
const POCKET_PACKAGE_VARIANT_SIZE = 40;
const POCKET_PACKAGE_SECTION_SIZE = 16;
const POCKET_PACKAGE_ALIGN = 16;

const POCKET_SECTION = {
  identity: 1,
  plan: 2,
  js: 3,
  pak: 4,
};

const FNV_OFFSET = 0xcbf29ce484222325n;
const FNV_PRIME = 0x100000001b3n;
const FNV_MASK = 0xffffffffffffffffn;

function fnv1a64(...chunks) {
  let h = FNV_OFFSET;
  for (const chunk of chunks) {
    for (let i = 0; i < chunk.length; i++) {
      h ^= BigInt(chunk[i]);
      h = (h * FNV_PRIME) & FNV_MASK;
    }
  }
  return h;
}

const align = (n) => (n + POCKET_PACKAGE_ALIGN - 1) & ~(POCKET_PACKAGE_ALIGN - 1);

function patchVaporHelperCode(code) {
  return code.replace(
    'if (i && i.appContext.vapor && p === "__vapor") {\n          return true;\n        }\n        return Reflect.get',
    'if (i && i.appContext.vapor && p === "__vapor") {\n          return true;\n        }\n        if (i && i.appContext.vapor && p === "inheritAttrs") {\n          return false;\n        }\n        return Reflect.get',
  );
}

const VAPOR_HELPERS = new Map([
  [propsHelperId, propsHelperCode],
  [vdomHelperId, vdomHelperCode],
  [vaporHelperId, patchVaporHelperCode(vaporHelperCode)],
]);

function collectFromSrc(dir) {
  const classes = new Set();
  const codepoints = new Set();
  function walk(d) {
    for (const f of fs.readdirSync(d)) {
      const full = path.join(d, f);
      if (fs.statSync(full).isDirectory()) {
        walk(full);
      } else if (/\.(jsx|tsx|js|ts)$/.test(f)) {
        const content = fs.readFileSync(full, 'utf8');
        const regex = /\b(?:class|className|cls)=["']([^"']+)["']/g;
        let m;
        while ((m = regex.exec(content)) !== null) {
          classes.add(m[1].trim());
        }
        for (let i = 0; i < content.length; i++) {
          const cp = content.codePointAt(i);
          if (cp && cp >= 32 && cp !== 127) {
            codepoints.add(cp);
          }
        }
      }
    }
  }
  walk(dir);
  return { classes, codepoints };
}

function encodeIdentity(identity) {
  const parts = [];
  for (const value of [identity.output, identity.id, identity.title]) {
    const utf8 = new TextEncoder().encode(value);
    const len = new Uint8Array(2);
    new DataView(len.buffer).setUint16(0, utf8.length, true);
    parts.push(len, utf8);
  }
  const total = parts.reduce((n, p) => n + p.length, 0);
  const out = new Uint8Array(total);
  let off = 0;
  for (const p of parts) {
    out.set(p, off);
    off += p.length;
  }
  return out;
}

function encodePocketPackage(pkg) {
  const variants = [...pkg.variants].sort((a, b) => (a.target < b.target ? -1 : 1));
  for (const v of variants) {
    v.sections = [...v.sections].sort((a, b) => a.kind - b.kind);
  }

  const manifestOff = POCKET_PACKAGE_HEADER_SIZE;
  const tableOff = align(manifestOff + pkg.manifest.length);
  const sectionTablesOff = tableOff + variants.length * POCKET_PACKAGE_VARIANT_SIZE;
  let cursor = sectionTablesOff;
  const sectionTableOffs = [];
  for (const v of variants) {
    sectionTableOffs.push(cursor);
    cursor += v.sections.length * POCKET_PACKAGE_SECTION_SIZE;
  }
  cursor = align(cursor);
  const payloadOffs = variants.map((v) =>
    v.sections.map((s) => {
      const off = cursor;
      cursor = align(cursor + s.bytes.length);
      return off;
    }),
  );
  const total = cursor + 8;

  const out = new Uint8Array(total);
  const dv = new DataView(out.buffer);
  dv.setUint32(0, POCKET_PACKAGE_MAGIC, true);
  dv.setUint32(4, POCKET_PACKAGE_VERSION, true);
  dv.setUint32(8, pkg.manifest.length, true);
  dv.setUint32(12, variants.length, true);
  out.set(pkg.manifest, manifestOff);

  variants.forEach((v, vi) => {
    const entry = tableOff + vi * POCKET_PACKAGE_VARIANT_SIZE;
    out.set(new TextEncoder().encode(v.target), entry);
    dv.setUint32(entry + 16, v.hostAbi, true);
    dv.setUint32(entry + 20, v.sections.length, true);
    dv.setUint32(entry + 24, sectionTableOffs[vi], true);
    dv.setBigUint64(entry + 32, fnv1a64(...v.sections.map((s) => s.bytes)), true);
    v.sections.forEach((s, si) => {
      const se = sectionTableOffs[vi] + si * POCKET_PACKAGE_SECTION_SIZE;
      dv.setUint32(se, s.kind, true);
      dv.setUint32(se + 8, payloadOffs[vi][si], true);
      dv.setUint32(se + 12, s.bytes.length, true);
      out.set(s.bytes, payloadOffs[vi][si]);
    });
  });

  dv.setBigUint64(total - 8, fnv1a64(out.subarray(0, total - 8)), true);
  return out;
}

function generateCHeader(bytes, varName = 'app_pocket') {
  let c = '// Auto-generated PocketJS binary package for ESP32-S3 (N16R8)\n';
  c += '#pragma once\n#include <stdint.h>\n#include <stddef.h>\n\n';
  c += 'const size_t ' + varName + '_len = ' + bytes.length + ';\n';
  c += 'const uint8_t ' + varName + '_data[' + bytes.length + '] = {\n  ';
  for (let i = 0; i < bytes.length; i++) {
    c += '0x' + bytes[i].toString(16).padStart(2, '0') + ', ';
    if ((i + 1) % 16 === 0) {
      c += '\n  ';
    }
  }
  c += '\n};\n';
  return c;
}

async function build() {
  const manifestPath = path.join(ROOT, 'pocket.json');
  let manifestRaw = '{}';
  if (fs.existsSync(manifestPath)) {
    manifestRaw = fs.readFileSync(manifestPath, 'utf8');
  }
  const manifest = JSON.parse(manifestRaw);
  const framework = manifest.app?.framework || 'vue-vapor';

  console.log('[PocketJS Build] Target: ESP32-S3 (N16R8) | Framework: ' + framework + ' | Toolchain: Node.js + pnpm');

  if (!fs.existsSync(DIST)) {
    fs.mkdirSync(DIST, { recursive: true });
  }

  // --- Tailwind Utilities Compilation ---
  console.log('[PocketJS Build] Collecting Tailwind classes and text codepoints...');
  const { classes, codepoints } = collectFromSrc(path.join(ROOT, 'src'));
  const compiledStyles = compileClasses(classes);
  console.log('[PocketJS Build] Compiled ' + Object.keys(compiledStyles.ids).length + ' Tailwind classes (' + compiledStyles.bin.length + ' bytes styles.bin)');

  const generatedStylesModuleSource = generateStylesModule(compiledStyles);
  fs.writeFileSync(path.join(DIST, 'styles.bin'), compiledStyles.bin);

  // --- Font Atlases Baking ---
  console.log('[PocketJS Build] Baking font atlases for slots: [' + compiledStyles.usedFontSlots.join(', ') + ']...');
  const atlases = await bakeAtlases({
    codepoints,
    slots: compiledStyles.usedFontSlots,
    rasterDensity: 1,
  });
  console.log('[PocketJS Build] Baked ' + atlases.length + ' font atlases successfully');

  // --- Image and SVG Assets Baking ---
  const imageDir = path.join(ROOT, 'src/assets/images');
  const imageBlobs = [];
  if (fs.existsSync(imageDir)) {
    for (const file of fs.readdirSync(imageDir)) {
      const full = path.join(imageDir, file);
      if (file.endsWith('.svg')) {
        const text = fs.readFileSync(full, 'utf8');
        const baked = bakeSvg(text, 1);
        imageBlobs.push({
          key: keyImage(file),
          dtype: PAK_DTYPE.img,
          data: encodeImageEntry(baked),
        });
      } else if (file.endsWith('.png')) {
        const raw = fs.readFileSync(full);
        const decoded = decodePng(raw);
        imageBlobs.push({
          key: keyImage(file),
          dtype: PAK_DTYPE.img,
          data: encodeImageEntry(decoded),
        });
      }
    }
  }
  console.log('[PocketJS Build] Baked ' + imageBlobs.length + ' image/spinner assets');

  // --- Resource Pak Packaging ---
  const pakBlobs = [
    { key: KEY_STYLES || 'ui:styles', dtype: 1, data: compiledStyles.bin },
    ...atlases.map((a) => ({
      key: 'ui:font.' + a.slot,
      dtype: 2,
      data: a.bytes,
    })),
    ...imageBlobs,
  ];
  const pakBytes = pack(pakBlobs);
  fs.writeFileSync(path.join(DIST, 'app.pak'), pakBytes);
  console.log('[PocketJS Build] Pak file generated: ' + path.join(DIST, 'app.pak') + ' (' + (pakBytes.length / 1024).toFixed(1) + ' KB)');

  const plugins = [];
  if (framework === 'vue-vapor') {
    plugins.push({
      name: 'vue-vapor',
      setup(buildContext) {
        buildContext.onResolve({ filter: /^\/vue-jsx-vapor\// }, (args) => ({
          path: args.path,
          namespace: 'vue-vapor-helper',
        }));

        buildContext.onLoad({ filter: /.*/, namespace: 'vue-vapor-helper' }, (args) => {
          const contents = VAPOR_HELPERS.get(args.path);
          if (!contents) return undefined;
          return { contents, loader: 'js', resolveDir: ROOT };
        });

        buildContext.onLoad({ filter: /styles\.generated\.ts$/ }, () => ({
          contents: generatedStylesModuleSource,
          loader: 'ts',
        }));

        buildContext.onLoad({ filter: /\.(jsx|tsx)$/ }, async (args) => {
          const source = await fs.promises.readFile(args.path, 'utf8');
          const transformed = transformVueJsxVapor(source, args.path, {}, false, false, false);
          return { contents: transformed.code, loader: 'js', resolveDir: path.dirname(args.path) };
        });
      },
    });
  } else {
    plugins.push({
      name: 'solid-universal',
      setup(buildContext) {
        buildContext.onLoad({ filter: /styles\.generated\.ts$/ }, () => ({
          contents: generatedStylesModuleSource,
          loader: 'ts',
        }));

        buildContext.onLoad({ filter: /\.(jsx|tsx)$/ }, async (args) => {
          const source = await fs.promises.readFile(args.path, 'utf8');
          const transformed = transformSync(source, {
            filename: args.path,
            presets: [
              ['babel-preset-solid', { generate: 'universal', moduleName: '@pocketjs/framework/solid/renderer' }],
            ],
          });
          return { contents: transformed.code, loader: 'js' };
        });
      },
    });
  }

  const jsOutfile = path.join(DIST, 'app.js');
  console.log('[PocketJS Build] Compiling JSX and bundling JavaScript...');
  await esbuild.build({
    entryPoints: [path.join(ROOT, manifest.app?.entry || 'src/index.jsx')],
    bundle: true,
    format: 'iife',
    globalName: 'RemapadApp',
    outfile: jsOutfile,
    plugins,
    platform: 'neutral',
    target: 'es2020',
    define: {
      'process.env.NODE_ENV': JSON.stringify('production'),
      'process.env': '{}',
      ...(framework === 'vue-vapor'
        ? { document: 'globalThis.__pocketDocument' }
        : {}),
    },
    minify: false,
  });

  const jsContent = fs.readFileSync(jsOutfile);
  console.log('[PocketJS Build] JS bundle generated: ' + jsOutfile + ' (' + (jsContent.length / 1024).toFixed(1) + ' KB)');

  const jsSectionBytes = new Uint8Array(jsContent.length + 1);
  jsSectionBytes.set(jsContent, 0);

  const identityBytes = encodeIdentity({
    output: manifest.app?.output || 'remapad-ui',
    id: manifest.id || 'com.remapad.esp32s3-ui',
    title: manifest.title || 'Remapad ESP32-S3 UI',
  });

  const planBytes = new TextEncoder().encode(JSON.stringify({
    target: 'esp32s3',
    display: { width: 240, height: 280, format: 'rgb565' },
    entry: manifest.app?.entry || 'src/index.jsx',
    framework,
  }));

  const pocketPkg = {
    manifest: new TextEncoder().encode(manifestRaw),
    variants: [
      {
        target: 'esp32s3',
        hostAbi: 1,
        sections: [
          { kind: POCKET_SECTION.identity, bytes: identityBytes },
          { kind: POCKET_SECTION.plan, bytes: planBytes },
          { kind: POCKET_SECTION.js, bytes: jsSectionBytes },
          { kind: POCKET_SECTION.pak, bytes: pakBytes },
        ],
      },
    ],
  };

  const packageBytes = encodePocketPackage(pocketPkg);
  const pocketOutfile = path.join(DIST, 'app.pocket');
  fs.writeFileSync(pocketOutfile, packageBytes);
  console.log('[PocketJS Build] Pocket package generated: ' + pocketOutfile + ' (' + (packageBytes.length / 1024).toFixed(1) + ' KB)');

  const binOutfile = path.join(DIST, 'app.bin');
  fs.writeFileSync(binOutfile, packageBytes);

  const cHeader = generateCHeader(packageBytes, 'app_pocket');
  const headerOutfile = path.join(DIST, 'app_pocket.h');
  fs.writeFileSync(headerOutfile, cHeader, 'utf8');
  console.log('[PocketJS Build] C header generated: ' + headerOutfile);

  if (fs.existsSync(FW_MAIN)) {
    const fwHeader = path.join(FW_MAIN, 'app_pocket.h');
    fs.writeFileSync(fwHeader, cHeader, 'utf8');
    console.log('[PocketJS Build] Synced C header to firmware: ' + fwHeader);
  }

  console.log('[PocketJS Build] Build completed successfully!');
}

build().catch((err) => {
  console.error('[PocketJS Build] Build failed:', err);
  process.exit(1);
});
