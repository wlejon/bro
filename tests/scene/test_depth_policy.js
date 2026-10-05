// The camera depth policy is one switch honoured everywhere: a frame rendered
// with BRO_DISABLE_REVERSED_Z=1 (conventional depth) must match the default
// reversed-Z frame — clears, compare ops, the MSAA depth resolve, the depth
// snapshot readers and the projections all flip together. The policy is fixed
// per process, so each side runs in a child bro-headless (fixture
// fixtures/depth_policy_scene.js).

const cp = require('child_process');
const path = require('path');

const exeName = process.platform === 'win32' ? 'bro-headless.exe' : 'bro-headless';
const exe = path.join(process.env.BRO_EXE_DIR, exeName);
const appDir = process.env.BRO_APP_DIR;
const fixture = path.join(appDir, '..', 'scene', 'fixtures', 'depth_policy_scene.js');

function frame(conventional) {
    const env = Object.assign({}, process.env);
    if (conventional) env.BRO_DISABLE_REVERSED_Z = '1';
    else delete env.BRO_DISABLE_REVERSED_Z;
    const r = cp.spawnSync(exe, [appDir, fixture], { encoding: 'utf8', env });
    const out = (r.stdout || '') + (r.stderr || '');
    if (out.includes('no Vulkan context') || out.includes('requires a GPU')) return null;
    assert(r.status === 0, 'fixture run failed (' + r.status + ')\n' + out);
    const line = out.split('\n').find(l => l.includes('DEPTH_POLICY_FRAME '));
    assert(line, 'fixture printed its frame\n' + out);
    return JSON.parse(line.slice(line.indexOf('DEPTH_POLICY_FRAME ') + 19));
}

const reversed = frame(false);
if (!reversed) {
    missingGpuContext('scene');
} else {
    const conventional = frame(true);
    assert(reversed.centre[0] > 120 && reversed.centre[0] > reversed.centre[2] * 2,
        'reversed-Z: the near red cube covers the centre (' + reversed.centre + ')');
    assert(conventional.centre[0] > 120 && conventional.centre[0] > conventional.centre[2] * 2,
        'conventional depth: the near red cube covers the centre (' + conventional.centre + ')');
    let maxDelta = 0;
    let sum = 0;
    for (let i = 0; i < reversed.grid.length; i++) {
        const d = Math.abs(reversed.grid[i] - conventional.grid[i]);
        maxDelta = Math.max(maxDelta, d);
        sum += d;
    }
    const mean = sum / reversed.grid.length;
    assert(maxDelta <= 6 && mean < 1.5,
        'both depth policies render the same frame (max delta ' + maxDelta + ', mean ' + mean.toFixed(2) + ')');
}
