// While only a compositor-carried animation runs, the raster thread keeps the
// base layer it already drew (each of its two pool copies remembers which base
// recording it holds) and draws just the promoted layer. When the base does
// change, both copies must pick it up: a copy left on the old base would show
// the old page on every other presented frame. presentedFrame() needs
// BRO_CAPTURE_PRESENTS=1, set by tests/run_tests.sh.

function px(img, x, y) {
    const i = (y * img.width + x) * 4;
    return [img.data[i], img.data[i + 1], img.data[i + 2]];
}
function near(p, rgb) {
    return Math.abs(p[0] - rgb[0]) <= 2 && Math.abs(p[1] - rgb[1]) <= 2 && Math.abs(p[2] - rgb[2]) <= 2;
}
function frames(n) {
    return new Promise((resolve) => {
        const step = () => (--n <= 0 ? resolve() : requestAnimationFrame(step));
        requestAnimationFrame(step);
    });
}
// Every presented frame for a while: [still box colour, spinner centre colour].
async function watch(ms) {
    const out = [];
    const until = Date.now() + ms;
    while (Date.now() < until) {
        await frames(1);
        const img = presentedFrame();
        if (img) out.push([px(img, 40, 40), px(img, 220, 120)]);
    }
    return out;
}

const RED = [255, 0, 0], BLUE = [0, 0, 255], GREEN = [0, 255, 0];

async function run() {
    await frames(20);
    if (!presentedFrame()) {
        assert(false, 'presentedFrame() returned nothing (BRO_CAPTURE_PRESENTS=1 and a readable swapchain needed)');
        return;
    }
    let seen = await watch(400);
    assert(seen.length > 4, `frames presented while the spinner turns: ${seen.length}`);
    assert(seen.every(([s]) => near(s, RED)), 'the still box stays red while only the spinner animates');
    assert(seen.every(([, c]) => near(c, GREEN)), 'the spinner shows on every frame');

    // A base change: every frame from shortly after shows it, on both copies.
    for (const [colour, name] of [[BLUE, 'blue'], [RED, 'red'], [BLUE, 'blue']]) {
        document.getElementById('still').style.background = `rgb(${colour})`;
        let first = -1;
        seen = await watch(500);
        for (let i = 0; i < seen.length; i++) if (near(seen[i][0], colour)) { first = i; break; }
        assert(first >= 0, `the still box turned ${name}`);
        const after = seen.slice(first);
        const stale = after.filter(([s]) => !near(s, colour)).length;
        assert(stale === 0, `once ${name}, the still box stays ${name} on every presented frame: ` +
                            `${stale} of ${after.length} showed the old base`);
        assert(seen.every(([, c]) => near(c, GREEN)), 'the spinner shows on every frame through the base change');
    }
    console.log('windowed promoted base: checked');
}

const watchdog = setTimeout(() => {
    assert(false, 'windowed promoted base test timed out');
    window.close();
}, 30000);
run().catch((e) => assert(false, 'uncaught: ' + e)).finally(() => {
    clearTimeout(watchdog);
    window.close();
});
