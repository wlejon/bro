// A scroll layer on CPU surfaces is rastered in place where only part of it
// changed (engine/scroll_layers.h, ScrollLayerSet::rasterPart): its rows
// changing on hover draw those rows again, not the layer, and every frame
// matches a full record and raster without layers. On the GPU a layer is
// rastered whole (a raster there costs about the same either way), so this
// runs tests/layout/scroll_layer_partial_cpu.js in a child bro-headless
// --no-gpu.

const cp = require('child_process');
const path = require('path');

const exeName = process.platform === 'win32' ? 'bro-headless.exe' : 'bro-headless';
const exe = path.join(process.env.BRO_EXE_DIR, exeName);
const driver = path.join(process.env.BRO_APP_DIR, '..', 'layout', 'scroll_layer_partial_cpu.js');
const r = cp.spawnSync(exe, ['--no-gpu', process.env.BRO_APP_DIR, driver], { encoding: 'utf8', env: process.env });
const out = (r.stdout || '') + (r.stderr || '');
const line = out.split(/\r?\n/).find((l) => l.includes('PARTIAL_CPU'));
assert(r.status === 0, `the --no-gpu run passed (exit ${r.status})\n` +
       out.split(/\r?\n/).filter((l) => /ASSERT|PARTIAL_CPU|ERROR/.test(l)).join('\n'));
assert(line, `the --no-gpu run reported:\n${out}`);
if (line) console.log(line.slice(line.indexOf('PARTIAL_CPU')));
