// bro.window.captureScreen(options): the desktop itself, every app's windows,
// from the OS (DXGI / GDI on Windows, ScreenCaptureKit on macOS,
// wlr-screencopy on Wayland) or the scanout under DRM. Headless still reads
// the real desktop where the machine has one; where it has none (a CI
// runner, a session off the console, macOS without the Screen Recording
// permission) the call is refused with a code saying why, and that refusal
// is what this checks there.
const fs = require('fs');
const os = require('os');
const path = require('path');

assert(typeof bro.window.captureDisplays === 'function', 'bro.window.captureDisplays exists');
assert(bro.window._captureDesktop === undefined, 'the native half is not left on bro.window');

const displays = bro.window.captureDisplays();
assert(Array.isArray(displays), 'captureDisplays() is an array');
for (const d of displays) {
    for (const k of ['index', 'x', 'y', 'width', 'height', 'scale'])
        assert(typeof d[k] === 'number', 'display.' + k + ' is a number: ' + JSON.stringify(d));
    assert(typeof d.name === 'string' && typeof d.primary === 'boolean', 'display name/primary: ' + JSON.stringify(d));
    assert(d.width > 0 && d.height > 0, 'a display has a size: ' + JSON.stringify(d));
}

function nonUniform(r) {
    const px = r.pixels;
    const step = Math.max(4, ((px.length / 4 / 4096) | 0) * 4);
    let seen = new Set();
    for (let i = 0; i < px.length && seen.size < 3; i += step) seen.add((px[i] << 16) | (px[i + 1] << 8) | px[i + 2]);
    return seen.size >= 2;
}

let done = false;
let failure = null;

(async () => {
    // Bad options are refused before anything is read.
    let bad = null;
    try { await bro.window.captureScreen({ display: 99 }); } catch (e) { bad = e; }
    if (displays.length) assert(bad && bad.code === 'EINVAL', 'an unknown display is EINVAL: ' + (bad && bad.message));
    bad = null;
    try { await bro.window.captureScreen({ rect: { x: 0, y: 0, width: 0, height: 5 } }); } catch (e) { bad = e; }
    assert(bad && bad.code === 'EINVAL', 'an empty rect is EINVAL: ' + (bad && bad.message));

    let r;
    try {
        r = await bro.window.captureScreen({});
    } catch (e) {
        assert(['ENOTSUP', 'EPERM'].includes(e.code), 'a refusal says why with a code: ' + e.code + ' ' + e.message);
        console.log('test_capture_desktop: SKIP (no desktop to read: ' + e.code + ': ' + e.message + ')');
        return;
    }
    assert(displays.length > 0, 'a desktop that was read has displays');
    let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
    for (const d of displays) {
        x0 = Math.min(x0, d.x); y0 = Math.min(y0, d.y);
        x1 = Math.max(x1, d.x + d.width); y1 = Math.max(y1, d.y + d.height);
    }
    assert(r.x === x0 && r.y === y0 && r.width === x1 - x0 && r.height === y1 - y0,
           'the whole desktop is the union of the displays: ' + JSON.stringify({ x: r.x, y: r.y, w: r.width, h: r.height }));
    assert(r.display === -1, 'all displays is display -1');
    assert(typeof r.backend === 'string' && r.backend.length > 0, 'the backend is named: ' + r.backend);
    assert(r.pixels instanceof Uint8Array && r.pixels.length === r.width * r.height * 4, 'RGBA pixels of the size');
    assert(nonUniform(r), 'the desktop is not one flat colour');
    console.log('test_capture_desktop: ' + r.width + 'x' + r.height + ' via ' + r.backend + ', ' + displays.length + ' display(s)');

    // One display, a rectangle of it, written as a PNG, no pixels returned.
    const d = displays[0];
    const out = path.join(os.tmpdir(), 'bro_capture_desktop_' + Date.now(), 'part.png');
    const part = await bro.window.captureScreen({ display: d.index, rect: { x: 10, y: 20, width: 64, height: 48 },
                                                  path: out, pixels: false });
    assert(part.x === d.x + 10 && part.y === d.y + 20 && part.width === 64 && part.height === 48,
           'the rectangle is relative to the display: ' + JSON.stringify(part));
    assert(part.display === d.index, 'the display is reported');
    assert(part.pixels === undefined && part.path === out, 'pixels:false leaves them out; the path is given');
    const png = fs.readFileSync(out);
    const be32 = (o) => ((png[o] << 24) | (png[o + 1] << 16) | (png[o + 2] << 8) | png[o + 3]) >>> 0;
    assert(png[1] === 0x50 && be32(16) === 64 && be32(20) === 48, 'the PNG is 64x48');
    fs.rmSync(path.dirname(out), { recursive: true, force: true });

    const primary = await bro.window.captureScreen({ display: 'primary' });
    const p = displays.find((x) => x.primary) || displays[0];
    assert(primary.width === p.width && primary.height === p.height, 'primary is the primary display');
})().then(() => { done = true; }, (e) => { failure = e; done = true; });

for (let i = 0; i < 200 && !done; ++i) advanceTime(16);
if (failure) throw failure;
assert(done, 'the captures settled');

// The `screen` capability: an app that does not declare it is refused
// before anything is read (a child run, granted nothing).
{
    const cp = require('child_process');
    const headless = [process.env.BRO_HEADLESS, process.execPath].filter(Boolean).find((p) => fs.existsSync(p));
    const probe = 'bro.window.captureScreen({}).then((r) => console.log("CAPTURE:ok"), ' +
                  '(e) => console.log("CAPTURE:" + e.code)); advanceTime(16); advanceTime(16);';
    const r = cp.spawnSync(headless, ['--no-splash', bro.app.dir, '-e', probe], { encoding: 'utf8', timeout: 120000 });
    const out = (r.stdout || '') + (r.stderr || '');
    assert(/CAPTURE:EACCES/.test(out), 'captureScreen without `screen` is refused with EACCES: ' + out.slice(-400));
}
console.log('test_capture_desktop OK');
