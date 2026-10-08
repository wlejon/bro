// Shell-drawn window frames and the shell host's stacking, with real Wayland
// clients: the headless engine runs the DRM shell host's compositor
// (BRO_HEADLESS_COMPOSITOR=1, set by run_tests.sh for this file), so frames,
// stacking and pointer routing go through the same code as a DRM session.
//
//   - a [data-window-frame=<id>] element is placed by the engine around its
//     window (outer rect = frame + decoration insets), stacked with it
//   - a press on a frame reaches the shell and raises / focuses its window;
//     bro.compositor.beginMove from that press drags the window, and the
//     frame follows
//   - a press on a client surface reaches the client, raising it
//   - frames are hidden for windows that are not decorated

if (typeof hostCompositorSocket !== 'function' || hostCompositorSocket() === '' ||
    !bro.compositor || !bro.compositor.available) {
    skipTest('needs BRO_HEADLESS_COMPOSITOR=1 and the Wayland server');
}

const cp = require('child_process');
const fs = require('fs');
const dir = process.env.BC_TEST_CLIENT_DIR || '../brocompositor/build-release/tests';
const clientBin = dir + '/bc_wl_client';
if (!fs.existsSync(clientBin)) skipTest('bc_wl_client not found (BC_TEST_CLIENT_DIR)');

const realSleep = (ms) => cp.execSync('sleep ' + (ms / 1000));
function waitFor(pred, what) {
    for (let i = 0; i < 150; ++i) {
        flush();
        if (pred()) return;
        realSleep(20);
    }
    throw new Error('timed out waiting for ' + what);
}

document.body.style.margin = '0';
document.body.innerHTML = `
  <style>
    #wall { position: fixed; inset: 0; background: #202838; z-index: 0; }
    #frames { position: fixed; inset: 0; z-index: 900; pointer-events: none; }
    [data-window-frame] { pointer-events: auto; background: #445; border-radius: 8px; }
    [data-window-frame][data-window-focused] { background: #88a; }
    .title { height: 30px; margin: 3px 6px 0; }
    #dock { position: fixed; left: 0; right: 0; bottom: 0; height: 40px; z-index: 1000; background: #111; }
  </style>
  <div id="wall"></div>
  <div id="frames"></div>
  <div id="dock"></div>
`;
const framesEl = document.getElementById('frames');

assert(bro.compositor.setDecorations({ insets: { left: 6, top: 36, right: 6, bottom: 6 },
                                       maximizedInsets: { top: 36 } }), 'setDecorations');
const deco = bro.compositor.getDecorations();
assert(deco.insets.top === 36 && deco.maximizedInsets.left === 0, 'decorations round-trip');

const env = Object.assign({}, process.env, { WAYLAND_DISPLAY: hostCompositorSocket() });
const children = [];
function spawnClient(appId, color, extra) {
    const child = cp.spawn(clientBin, ['--app-id', appId, '--size', '400x300', '--color', color, '--dmabuf']
                           .concat(extra || []), { env, stdio: 'pipe' });
    children.push(child);
    const before = bro.compositor.getWindows().length;
    waitFor(() => bro.compositor.getWindows().length > before, appId + ' to map');
    return bro.compositor.getWindows().find((w) => w.appId === appId);
}

let exitCode = 0;
try {
    const a = spawnClient('frames-a', 'FFCC3333');
    const b = spawnClient('frames-b', 'FF33CC33');
    const c = spawnClient('frames-c', 'FF3333CC', ['--csd']);
    waitFor(() => bro.compositor.getWindow(c.id).focused, 'the newest window focused');
    assert(bro.compositor.getStacking().join() === [a.id, b.id, c.id].join(), 'newest on top');

    bro.compositor.moveWindow(a.id, { x: 100, y: 100, width: 400, height: 300 });
    bro.compositor.moveWindow(b.id, { x: 300, y: 200, width: 400, height: 300 });
    bro.compositor.moveWindow(c.id, { x: 900, y: 150, width: 400, height: 300 });
    waitFor(() => bro.compositor.getWindow(b.id).frame.x === 300, 'placement');

    // One frame element per window, the shell's own markup.
    const frames = {};
    for (const w of [a, b, c]) {
        const f = document.createElement('div');
        f.setAttribute('data-window-frame', String(w.id));
        const t = document.createElement('div');
        t.className = 'title';
        t.addEventListener('mousedown', () => bro.compositor.beginMove(w.id));
        f.appendChild(t);
        framesEl.appendChild(f);
        frames[w.id] = f;
    }
    flush();

    // Placed around the window by the engine.
    const rb = frames[b.id].getBoundingClientRect();
    assert(rb.left === 294 && rb.top === 164 && rb.width === 412 && rb.height === 342,
           'frame b outer rect: ' + JSON.stringify(rb));
    assert(frames[c.id].style.display === 'none', 'a client-side-decorated window has no frame');
    assert(bro.compositor.getWindow(c.id).decorated === false, 'c is not decorated');
    assert(bro.compositor.getWindow(a.id).decorated === true, 'a is decorated');
    assert(Number(frames[b.id].style.zIndex) > Number(frames[a.id].style.zIndex), 'frame order follows the stack');

    // Click a's client area where b does not cover it: the client gets it,
    // and a is raised and focused.
    assert(hostPointer('down', 150, 200) === false, 'a press on a client goes to the client');
    hostPointer('up', 150, 200);
    waitFor(() => bro.compositor.getWindow(a.id).focused, 'a focused by click');
    assert(bro.compositor.getStacking().at(-1) === a.id, 'clicking a raises it');
    flush();
    assert(frames[a.id].hasAttribute('data-window-focused'), 'focused frame marked');
    assert(!frames[b.id].hasAttribute('data-window-focused'), 'unfocused frame not marked');
    assert(Number(frames[a.id].style.zIndex) > Number(frames[b.id].style.zIndex), 'a frame raised with a');

    // b's title bar, where a (now on top) does not cover it.
    const tx = 600, ty = 180;
    assert(hostPointer('down', tx, ty) === true, 'a press on a frame goes to the shell');
    waitFor(() => bro.compositor.getWindow(b.id).focused, 'b focused by its frame');
    assert(bro.compositor.getStacking().at(-1) === b.id, 'pressing a frame raises its window');
    assert(bro.compositor.getDrag() && bro.compositor.getDrag().windowId === b.id, 'beginMove armed');
    assert(hostPointer('move', tx + 50, ty + 40) === false, 'the drag takes the pointer');
    assert(hostPointer('move', tx + 120, ty + 90) === false, 'the drag takes the pointer');
    const moved = bro.compositor.getWindow(b.id).frame;
    assert(moved.x === 420 && moved.y === 290, 'window followed the drag: ' + JSON.stringify(moved));
    flush();
    const rb2 = frames[b.id].getBoundingClientRect();
    assert(rb2.left === 414 && rb2.top === 254, 'frame followed its window: ' + JSON.stringify(rb2));
    assert(hostPointer('up', tx + 120, ty + 90) === true, 'the release reaches the shell that began the drag');
    assert(!bro.compositor.getDrag(), 'drag over');

    // The dock (z-index 1000) is the shell's, over any window.
    const vh = window.innerHeight;
    bro.compositor.moveWindow(a.id, { x: 100, y: vh - 200, width: 400, height: 300 });
    waitFor(() => bro.compositor.getWindow(a.id).frame.y === vh - 200, 'a moved under the dock');
    assert(hostPointer('down', 200, vh - 20) === true, 'the dock over a window is the shell\'s');
    hostPointer('up', 200, vh - 20);
    assert(hostPointer('down', 200, vh - 100) === false, 'above the dock the window is the client\'s');
    hostPointer('up', 200, vh - 100);

    // Maximized: the frame keeps its title bar inside the work area.
    assert(bro.compositor.maximizeWindow(b.id), 'maximize');
    waitFor(() => bro.compositor.getWindow(b.id).maximized, 'b maximized');
    flush();
    assert(frames[b.id].getAttribute('data-window-state') === 'maximized', 'maximized state attribute');
    assert(frames[b.id].getBoundingClientRect().top === 0, 'maximized frame at the top of the work area');
    assert(!frames[b.id].hasAttribute('data-window-borderless'), 'a title bar is not borderless');

    // Borderless maximize: zero maximized insets. The client fills the work
    // area from its top-left pixel, and the frame stays, covering exactly it.
    bro.compositor.setDecorations({ maximizedInsets: 0 });
    waitFor(() => bro.compositor.getWindow(b.id).frame.y === 0, 'b re-fitted without a title bar');
    flush();
    const bw = bro.compositor.getWindow(b.id);
    assert(bw.framed && bw.borderless, 'b framed and borderless: ' + JSON.stringify(bw));
    assert(bw.frame.x === 0 && bw.frame.width === window.innerWidth, 'b fills the width: ' + JSON.stringify(bw.frame));
    assert(frames[b.id].style.display !== 'none', 'a borderless window keeps its frame');
    assert(frames[b.id].hasAttribute('data-window-borderless'), 'borderless attribute');
    const rbl = frames[b.id].getBoundingClientRect();
    assert(rbl.left === 0 && rbl.top === 0 && rbl.width === bw.frame.width && rbl.height === bw.frame.height,
           'the borderless frame covers exactly the client: ' + JSON.stringify(rbl));
    assert(!bro.compositor.getWindow(a.id).borderless, 'a normal window is not borderless');

    // A press near its top is the client's: no title band out of the policy.
    assert(hostPointer('down', 600, 10) === false, 'a borderless client keeps its top edge');
    hostPointer('up', 600, 10);
    assert(!bro.compositor.getDrag(), 'no move armed on a borderless client');

    // An overlay in the frame is over the client and takes the pointer first.
    const overlay = document.createElement('div');
    overlay.setAttribute('data-window-overlay', '');
    overlay.style.cssText = 'position: absolute; top: 0; right: 0; width: 24px; height: 24px; background: #f0f';
    let overlayDowns = 0;
    overlay.addEventListener('mousedown', () => ++overlayDowns);
    frames[b.id].appendChild(overlay);
    flush();
    const ox = window.innerWidth - 10;
    assert(hostPointer('down', ox, 10) === true, 'the overlay over the client is the shell\'s');
    hostPointer('up', ox, 10);
    assert(overlayDowns === 1, 'the overlay got the press');
    assert(hostPointer('down', ox, 40) === false, 'below the overlay the client has it');
    hostPointer('up', ox, 40);
    // Back to a title bar: the attribute goes.
    bro.compositor.setDecorations({ maximizedInsets: { top: 36 } });
    waitFor(() => bro.compositor.getWindow(b.id).frame.y === 36, 'title bar back');
    flush();
    assert(!frames[b.id].hasAttribute('data-window-borderless'), 'borderless attribute cleared');

    if (process.env.BRO_FRAMES_SCREENSHOT) screenshot(process.env.BRO_FRAMES_SCREENSHOT);
} catch (e) {
    console.error(String(e && e.stack || e));
    exitCode = 1;
} finally {
    for (const ch of children) ch.kill();
}
if (exitCode) throw new Error('test_window_frames failed');
