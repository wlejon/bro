// <terminal> in the windowed frame loop: the engine pumps the terminal every
// frame (its parser thread publishes frames on its own; nothing wakes the
// main thread), so output fed into it, and output a real child process
// writes through the PTY, reach the presented swapchain image without any
// script involvement.

function px(img, x, y) {
    const i = (y * img.width + x) * 4;
    return [img.data[i], img.data[i + 1], img.data[i + 2], img.data[i + 3]];
}
function frames(n) {
    return new Promise((resolve) => {
        const step = () => (--n <= 0 ? resolve() : requestAnimationFrame(step));
        requestAnimationFrame(step);
    });
}
async function until(pred, maxFrames) {
    for (let i = 0; i < maxFrames; ++i) {
        if (pred()) return true;
        await frames(1);
    }
    return pred();
}

async function run() {
    if (!bro.terminal || !bro.terminal.available) {
        skipTest('<terminal> is compiled out of this build (BRO_WITH_TERMINAL)');
        return;
    }
    const t = document.getElementById('t');
    await frames(5);
    const r = t.getBoundingClientRect();
    const cellW = r.width / 20, cellH = r.height / 6;
    const cell = (col, row) => [Math.floor(r.left + cellW * (col + 0.5)), Math.floor(r.top + cellH * (row + 0.5))];

    // Fed output: a red block at the top-left.
    t.feed('\x1b[41m    \x1b[0m');
    let img = null;
    const red = await until(() => {
        img = presentedFrame();
        if (!img) return false;
        const p = px(img, ...cell(1, 0));
        return p[0] > 150 && p[1] < 80 && p[2] < 80;
    }, 60);
    if (!img) {
        assert(false, 'presentedFrame() returned nothing (BRO_CAPTURE_PRESENTS=1 needed)');
        return;
    }
    assert(red, 'fed output is presented: got (' + px(img, ...cell(1, 0)) + ')');
    const bg = px(img, ...cell(15, 3));
    assert(bg[0] < 30 && bg[1] < 30 && bg[2] < 30, 'terminal background elsewhere: (' + bg + ')');

    // A child's output through the PTY: a green block on the second row.
    const path = require('path');
    const fs = require('fs');
    const child = path.join(path.dirname(process.execPath),
                            process.platform === 'win32' ? 'bro_pty_child.exe' : 'bro_pty_child');
    if (fs.existsSync(child)) {
        t.spawn({ command: child, args: ['print', '\\n\\e[42m    \\e[0m'] });
        const green = await until(() => {
            img = presentedFrame();
            const p = img ? px(img, ...cell(1, 1)) : [0, 0, 0];
            // The palette's green is (13,188,121): green-dominant, not pure.
            return p[1] > 150 && p[1] > p[0] + 60 && p[1] > p[2] + 40;
        }, 300);
        assert(green, 'a child\'s output is presented: got (' + (img ? px(img, ...cell(1, 1)) : 'none') + ')');
        await until(() => !t.running, 300);
        assert(!t.running && t.exitCode === 0, 'the child exited 0');
    }
}

const watchdog = setTimeout(() => {
    assert(false, 'windowed terminal test timed out');
    window.close();
}, 30000);
run().catch((e) => assert(false, 'uncaught: ' + e)).finally(() => {
    clearTimeout(watchdog);
    window.close();
});
