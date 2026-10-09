// A drag that starts in the shell's own page and goes to a Wayland client:
// the shell host offers the page's drag data as the compositor's own
// wl_data_source (Engine::startShellDrag), so a client under the pointer gets
// wl_data_device enter / motion / leave and, on the release, the drop and
// the data; the page hears dragend with the action the client took. Back
// over the shell the drag is the page's own again (dragenter / dragover /
// drop there). Run in the headless shell host (BRO_HEADLESS_COMPOSITOR=1,
// set by run_tests.sh) against bc_wl_client --drop, a real data-device client.

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
  <style>
    #wall { position: fixed; inset: 0; background: rgb(200, 200, 200); z-index: 0; }
    #src { position: fixed; left: 40px; top: 40px; width: 120px; height: 40px; background: #46a; z-index: 1; }
    #target { position: fixed; left: 40px; top: 400px; width: 160px; height: 100px; background: #6a4; z-index: 1; }
  </style>
  <div id="wall"></div>
  <div id="src" draggable="true">drag me</div>
  <div id="target"></div>
`;
const src = document.getElementById('src');
const target = document.getElementById('target');
const log = [];
let dragend = null, dropped = null;
src.addEventListener('dragstart', (e) => {
    e.dataTransfer.setData('text/plain', 'from the shell');
    e.dataTransfer.effectAllowed = 'copy';
    log.push('dragstart');
});
src.addEventListener('dragend', (e) => { dragend = e.dataTransfer.dropEffect; log.push('dragend'); });
target.addEventListener('dragenter', (e) => { e.preventDefault(); log.push('target-dragenter'); });
target.addEventListener('dragover', (e) => { e.preventDefault(); e.dataTransfer.dropEffect = 'copy'; });
target.addEventListener('drop', (e) => {
    e.preventDefault();
    dropped = e.dataTransfer.getData('text/plain');
    log.push('drop');
});

const env = Object.assign({}, process.env, { WAYLAND_DISPLAY: hostCompositorSocket() });
const child = cp.spawn(clientBin, ['--app-id', 'taker', '--size', '400x300', '--color', 'FF303030', '--dmabuf', '--drop'],
                       { env, stdio: 'pipe', encoding: 'utf8' });
let out = '';
child.stdout.on('data', (chunk) => { out += chunk; });
const count = (s) => out.split('\n').filter((l) => l.startsWith(s)).length;

let exitCode = 0;
try {
    waitFor(() => bro.compositor.getWindows().some((w) => w.appId === 'taker'), 'the window to map');
    const win = bro.compositor.getWindows().find((w) => w.appId === 'taker');
    assert(bro.compositor.moveWindow(win.id, { x: 400, y: 150, width: 400, height: 300 }), 'moveWindow');
    waitFor(() => bro.compositor.getWindow(win.id).frame.x === 400, 'the window to move');

    // 1. Out of the page onto the client, dropped there.
    assert(hostPointer('down', 60, 60) === true, 'the press on the shell is the shell\'s');
    hostPointer('move', 70, 70);
    hostPointer('move', 90, 90);
    assert(log.includes('dragstart'), 'dragstart: ' + log.join(','));
    // The label follows the pointer, over the shell.
    flush();
    const label = getPixel(90 + 14 + 4, 90 + 14 + 13);
    assert(label.r < 100 && label.g < 100, 'the drag label below right of the pointer: ' + JSON.stringify(label));

    hostPointer('move', 500, 250);
    waitFor(() => out.includes('drag-enter 100 100'), 'the client\'s drag-enter at 100, 100');
    waitFor(() => out.includes('drag-action 1'), 'the client to take it as a copy');
    hostPointer('move', 510, 260);
    waitFor(() => out.includes('drag-motion 110 110'), 'the client\'s drag-motion');
    hostPointer('up', 510, 260);
    waitFor(() => out.includes('dropped from the shell'), 'the client to read the dropped text');
    waitFor(() => dragend !== null, 'dragend in the page');
    assert(dragend === 'copy', 'dragend says the client copied it: ' + dragend);
    assert(dropped === null, 'the page itself got no drop');

    // 2. Onto the client and back, dropped in the page.
    dragend = null;
    log.length = 0;
    hostPointer('down', 60, 60);
    hostPointer('move', 70, 70);
    hostPointer('move', 90, 90);
    assert(log.includes('dragstart'), 'second dragstart: ' + log.join(','));
    hostPointer('move', 520, 270);
    waitFor(() => count('drag-enter') === 2, 'the second drag-enter');
    hostPointer('move', 100, 450);
    waitFor(() => out.includes('drag-leave'), 'the client\'s drag-leave');
    hostPointer('move', 110, 455);
    assert(log.includes('target-dragenter'), 'the page\'s drag is back over its target: ' + log.join(','));
    hostPointer('up', 110, 455);
    waitFor(() => dragend !== null, 'the second dragend');
    assert(dropped === 'from the shell', 'the page dropped its own data: ' + dropped);
    assert(dragend === 'copy', 'dragend after the page\'s drop: ' + dragend);
    assert(count('dropped') === 1, 'the client got no second drop');

    // 3. The pointer is the shell's again afterwards.
    assert(hostPointer('down', 60, 60) === true, 'a press on the shell after the drags');
    hostPointer('up', 60, 60);
} catch (e) {
    console.error(String(e && e.stack || e));
    console.error('client: ' + out);
    exitCode = 1;
} finally {
    child.kill();
}
if (exitCode) throw new Error('test_shell_drag failed');
}  // run
