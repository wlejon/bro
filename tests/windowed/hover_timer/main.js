// A tooltip shows while the pointer rests on a button's icon, in the windowed
// loop. Toolbar tooltips (helmapps lib/tooltip.js, inlined here) start a
// 550 ms timer on pointerover and cancel it when the pointer reaches anything
// outside the [data-tip] element; a hand on a mouse is never quite still, so
// the pointer jitters a pixel or two over the icon meanwhile. Each move must
// keep the same hover target chain, and the tip must reach the screen.

let tip = null, timer = null, current = null;
const events = [];
function hide() {
    if (timer) { clearTimeout(timer); timer = null; }
    if (tip) { tip.remove(); tip = null; }
    current = null;
}
function show(el) {
    hide();
    current = el;
    timer = setTimeout(() => {
        if (current !== el) return;
        tip = document.createElement('div');
        tip.className = 'tooltip';
        document.body.appendChild(tip);
    }, 550);
}
document.addEventListener('pointerover', (e) => {
    const el = e.target && e.target.closest ? e.target.closest('[data-tip]') : null;
    events.push(e.target.nodeName);
    if (el === current) return;
    if (el) show(el); else hide();
});

function px(img, x, y) {
    const i = (y * img.width + x) * 4;
    return [img.data[i], img.data[i + 1], img.data[i + 2]];
}
function frames(n) {
    return new Promise((resolve) => {
        const step = () => (--n <= 0 ? resolve() : requestAnimationFrame(step));
        requestAnimationFrame(step);
    });
}

async function run() {
    await frames(10);
    // Rest on the icon in the middle of the button, jittering by a pixel a
    // frame for a second.
    for (let i = 0; i < 60; ++i) {
        mouseMove(60 + (i % 3) - 1, 60 + ((i >> 1) % 3) - 1);
        await frames(1);
    }
    assert(document.querySelector('.tooltip'), `the tip shows while the pointer rests on the icon (pointerover targets: ${events.join(' ')})`);
    let img = null;
    const deadline = Date.now() + 2000;
    for (;;) {
        img = presentedFrame();
        if (img && px(img, 60, 110)[0] > 200) break;
        if (Date.now() > deadline) break;
        await frames(1);
    }
    assert(img && px(img, 60, 110)[0] > 200, `the tip reached the screen (got ${img ? px(img, 60, 110) : 'no frame'})`);
    console.log(`hover timer: pointerover targets ${events.join(' ')}`);
}

run().catch((e) => assert(false, String(e && e.stack || e))).finally(() => window.close());
