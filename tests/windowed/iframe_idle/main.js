// An iframe that changes nothing is neither re-recorded nor re-replayed: the
// frame it last published stands. This checks the other half on the presented
// swapchain image: whatever does change in it (its DOM, :hover inside it, its
// box) still reaches the screen, and it keeps showing while it idles and the
// parent repaints around it. presentedFrame() needs BRO_CAPTURE_PRESENTS=1,
// set by tests/run_tests.sh. A rAF callback is not a present (they run well
// ahead of the swapchain here), so each check polls the presented frame.

function px(img, x, y) {
    const i = (y * img.width + x) * 4;
    return [img.data[i], img.data[i + 1], img.data[i + 2], img.data[i + 3]];
}
function isNear(img, x, y, rgb, tol) {
    const p = px(img, x, y);
    return Math.abs(p[0] - rgb[0]) <= tol && Math.abs(p[1] - rgb[1]) <= tol && Math.abs(p[2] - rgb[2]) <= tol;
}
function frames(n) {
    return new Promise((resolve) => {
        const step = () => (--n <= 0 ? resolve() : requestAnimationFrame(step));
        requestAnimationFrame(step);
    });
}
function wait(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms));
}
// Poll the presented frame until every [x, y, rgb, what] check holds (2 s).
async function shows(checks) {
    const deadline = Date.now() + 2000;
    let img = null;
    for (;;) {
        img = presentedFrame();
        if (img && checks.every(([x, y, rgb]) => isNear(img, x, y, rgb, 2))) return;
        if (Date.now() > deadline) break;
        await frames(1);
    }
    if (!img) { assert(false, 'presentedFrame() returned nothing'); return; }
    for (const [x, y, rgb, what] of checks)
        assert(isNear(img, x, y, rgb, 2), `${what} at (${x},${y}): expected ~(${rgb}), got (${px(img, x, y)})`);
}

const RED = [255, 0, 0], BLUE = [0x22, 0x44, 0xaa], GREY = [0x88, 0x88, 0x88];
const YELLOW = [255, 255, 0], CYAN = [0, 255, 255], PAGE = [0x20, 0x30, 0x40];
const GREEN = [0, 255, 0], MAGENTA = [255, 0, 255];

async function run() {
    const frame = document.getElementById('frame');
    const outer = document.getElementById('outer');
    await frames(20);
    if (!presentedFrame()) {
        assert(false, 'presentedFrame() returned nothing (BRO_CAPTURE_PRESENTS=1 and a readable swapchain needed)');
        return;
    }
    // The iframe sits at (10,100) 120x60: #box at its (0,0), #hov at (60,0).
    await shows([[30, 120, RED, 'iframe #box'], [90, 120, GREY, 'iframe #hov'],
                 [120, 150, BLUE, 'iframe background'], [220, 30, GREEN, 'parent element']]);

    // Idle, while the parent repaints around it.
    for (let i = 0; i < 6; ++i) {
        const colour = i % 2 ? GREEN : MAGENTA;
        outer.style.background = `rgb(${colour})`;
        await shows([[220, 30, colour, 'parent repainted'],
                     [30, 120, RED, 'idle iframe still shown'],
                     [120, 150, BLUE, 'idle iframe background still shown']]);
    }

    // A DOM change inside it.
    const sub = frame.contentDocument;
    assert(sub && sub.getElementById('box'), 'iframe exposes its contentDocument');
    sub.getElementById('box').style.background = '#00ffff';
    await shows([[30, 120, CYAN, 'iframe DOM change reached the screen']]);

    // Idle again after the change: the new frame stands.
    await wait(200);
    await shows([[30, 120, CYAN, 'changed iframe still shown while idle'],
                 [120, 150, BLUE, 'changed iframe background still shown while idle']]);

    // :hover inside it, and off it again.
    mouseMove(90, 120);
    await shows([[90, 120, YELLOW, 'iframe :hover reached the screen']]);
    mouseMove(300, 220);
    await shows([[90, 120, GREY, 'iframe :hover cleared']]);

    // Its box grows: the new area is drawn by the sub-document.
    frame.style.width = '180px';
    await shows([[170, 150, BLUE, 'grown iframe draws its new area'],
                 [30, 120, CYAN, 'grown iframe keeps its content'],
                 [300, 220, PAGE, 'page background']]);
    console.log('windowed iframe idle: checked');
}

// A hang must not outlive the runner's timeout silently.
const watchdog = setTimeout(() => {
    assert(false, 'windowed iframe idle test timed out');
    window.close();
}, 30000);
run().catch((e) => assert(false, 'uncaught: ' + e)).finally(() => {
    clearTimeout(watchdog);
    window.close();
});
