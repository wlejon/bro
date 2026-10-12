// The page's own partial raster (Engine::replayAppLayers) is its whole
// raster, pixel for pixel, on the GPU and the CPU: it draws the damage and
// everything that reaches it whole, with the rounded clips over them
// (render::coveringBounds), in a scratch the pool surface's size, and puts
// back the damage. In a scratch only the area's size (at the area's corner)
// rounded edges came out a level apart on the GPU (278 pixels here: the GPU
// maps positions through the target's size). Runs
// tests/layout/page_partial_verify.js here and in a child bro-headless
// --no-gpu.

const cp = require('child_process');
const path = require('path');

const driver = path.join(process.env.BRO_APP_DIR, '..', 'layout', 'page_partial_verify.js');
const exeName = process.platform === 'win32' ? 'bro-headless.exe' : 'bro-headless';
const exe = path.join(process.env.BRO_EXE_DIR, exeName);
for (const flags of [[], ['--no-gpu']]) {
    const r = cp.spawnSync(exe, [...flags, '--width', '1280', '--height', '800', process.env.BRO_APP_DIR, driver],
                           { encoding: 'utf8', env: process.env });
    const out = (r.stdout || '') + (r.stderr || '');
    const line = out.split(/\r?\n/).find((l) => l.includes('PAGE_PARTIAL'));
    const where = flags.length ? '--no-gpu' : 'default';
    assert(r.status === 0, `${where}: the run passed (exit ${r.status})\n` +
           out.split(/\r?\n/).filter((l) => /ASSERT|PAGE_PARTIAL|ERROR/.test(l)).join('\n'));
    if (line) console.log(where + ': ' + line.slice(line.indexOf('PAGE_PARTIAL')));
}
