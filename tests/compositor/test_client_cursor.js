// A client's own cursor picture (wl_pointer.set_cursor with a surface), in
// the shell host: drawn with its hotspot on the pointer while the pointer is
// over that client, following the client's updates to the surface (a new
// buffer, wl_surface.offset moving the hotspot), and gone once the pointer
// leaves it. The headless shell host (BRO_HEADLESS_COMPOSITOR=1, set by
// run_tests.sh) has no cursor plane, so the picture is drawn into the frame
// that getPixel reads: the path a DRM session takes for a cursor the plane
// cannot hold.

const cp = require('child_process');
const fs = require('fs');
const dir = process.env.BC_TEST_CLIENT_DIR || '../brocompositor/build-release/tests';
const clientBin = dir + '/bc_wl_client';

if (typeof hostCompositorSocket !== 'function' || hostCompositorSocket() === '' ||
    !bro.compositor || !bro.compositor.available) {
    skipTest('needs BRO_HEADLESS_COMPOSITOR=1 and the Wayland server');
} else if (!fs.existsSync(clientBin)) {
    skipTest('bc_wl_client not found (BC_TEST_CLIENT_DIR)');
} else {
    run();
}

function run() {

// advanceTime delivers the client's output (child_process events), wallSleep
// gives the client and the compositor real time.
function waitFor(pred, what) {
    for (let i = 0; i < 150; ++i) {
        advanceTime(16);
        if (pred()) return;
        wallSleep(20);
    }
    throw new Error('timed out waiting for ' + what);
}

document.body.style.margin = '0';
document.body.innerHTML = `
  <style>#wall { position: fixed; inset: 0; background: rgb(32, 40, 56); z-index: 0; }</style>
  <div id="wall"></div>
`;

const env = Object.assign({}, process.env, { WAYLAND_DISPLAY: hostCompositorSocket() });
// 12 x 10 magenta, hotspot 3, 4.
const child = cp.spawn(clientBin, ['--app-id', 'cursor', '--size', '400x300', '--color', 'FF303030', '--dmabuf',
                                   '--cursor', '12x10:FFFF00FF:3,4'], { env, stdio: 'pipe', encoding: 'utf8' });
let out = '';
child.stdout.on('data', (chunk) => { out += chunk; });

const near = (p, r, g, b) => Math.abs(p.r - r) < 24 && Math.abs(p.g - g) < 24 && Math.abs(p.b - b) < 24;
let exitCode = 0;
try {
    waitFor(() => bro.compositor.getWindows().some((w) => w.appId === 'cursor'), 'the window to map');
    const win = bro.compositor.getWindows().find((w) => w.appId === 'cursor');
    assert(bro.compositor.moveWindow(win.id, { x: 200, y: 150, width: 400, height: 300 }), 'moveWindow');
    waitFor(() => bro.compositor.getWindow(win.id).frame.x === 200, 'the window to move');

    // Onto the client: it sets its cursor on the pointer's enter.
    const px = 400, py = 300;
    hostPointer('move', px - 10, py - 10);
    hostPointer('move', px, py);
    waitFor(() => out.includes('cursor-set'), 'the client to set its cursor');
    waitFor(() => near(getPixel(px - 3, py - 4), 255, 0, 255), 'the cursor picture at the pointer');
    assert(near(getPixel(px - 3 + 11, py - 4 + 9), 255, 0, 255), 'its far corner (12 x 10)');
    assert(near(getPixel(px - 4, py - 4), 48, 48, 48), 'left of it, the client');
    assert(near(getPixel(px - 3, py - 5), 48, 48, 48), 'above it, the client');
    assert(near(getPixel(px + 9, py + 6), 48, 48, 48), 'past it, the client');

    // It follows the pointer.
    hostPointer('move', px + 50, py + 20);
    flush();
    assert(near(getPixel(px + 50 - 3, py + 20 - 4), 255, 0, 255), 'moved with the pointer');
    assert(near(getPixel(px - 3, py - 4), 48, 48, 48), 'and gone from where it was');

    // A new picture, 16 x 14 cyan, attached 2, 1 in: the hotspot moves to 1, 3.
    child.stdin.write('cursor 16 14 FF00FFFF 2 1\n');
    waitFor(() => out.includes('cursor-updated'), 'the client to update its cursor');
    const qx = px + 50, qy = py + 20;
    waitFor(() => near(getPixel(qx - 1, qy - 3), 0, 255, 255), 'the new picture, at its new hotspot');
    assert(near(getPixel(qx - 1 + 15, qy - 3 + 13), 0, 255, 255), 'its far corner (16 x 14)');
    assert(near(getPixel(qx - 2, qy - 3), 48, 48, 48), 'left of it, the client');

    // Off the client, onto the shell: the client's picture goes.
    hostPointer('move', 100, 80);
    flush();
    assert(near(getPixel(100 - 1, 80 - 3), 32, 40, 56), 'no client cursor over the shell');
    assert(near(getPixel(qx - 1, qy - 3), 48, 48, 48), 'nor where it was');
} catch (e) {
    console.error(String(e && e.stack || e));
    exitCode = 1;
} finally {
    child.kill();
}
if (exitCode) throw new Error('test_client_cursor failed');
}  // run
