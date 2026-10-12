// A button re-rendered under a still pointer stays hovered, in the windowed
// loop: the frame loop hit-tests again after the layout the change brought
// (Engine::recheckHover), with no input and no requestAnimationFrame loop.
// The headless version, with iframes, windows and scrolls, is
// tests/dom/test_hover_recheck.js.

const events = [];
for (const t of ['pointerover', 'pointerout', 'mouseover', 'pointermove', 'mousemove'])
    document.addEventListener(t, (e) => events.push(t + ' ' + e.target.id));

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
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function run() {
    await frames(10);
    mouseMove(80, 60);
    await sleep(200);
    assert(document.getElementById('b1').matches(':hover'), 'the pointer hovers b1');
    events.length = 0;

    // Re-rendered from a timer, as an app re-rendering on its own would.
    setTimeout(() => { document.getElementById('host').innerHTML = '<button class="b" id="b2"></button>'; }, 50);
    await sleep(600);
    const b2 = document.getElementById('b2');
    const got = events.join(', ');
    assert(b2 && b2.matches(':hover'), `the button re-rendered under the still pointer is :hover (${got})`);
    assert(got.includes('pointerover b2') && got.includes('mouseover b2'), `with pointerover and mouseover (${got})`);
    assert(!got.includes('move'), `and no move: the pointer did not move (${got})`);
    const img = presentedFrame();
    assert(img && px(img, 80, 60)[0] > 200 && px(img, 80, 60)[1] < 60,
           `its hover reached the screen (got ${img ? px(img, 80, 60) : 'no frame'})`);
    console.log(`hover recheck: ${got}`);
}

run().catch((e) => assert(false, String(e && e.stack || e))).finally(() => window.close());
