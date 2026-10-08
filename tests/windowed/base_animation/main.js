// An animation the compositor cannot carry (it animates `left`, a layout
// property, so the element is not promoted to a layer of its own) has to
// reach the screen on every frame: its new value is in the base layer, so the
// base has to be re-recorded each frame it runs. The layout pass that advances
// it also clears the document's dirty flag, so the frame loop learns of it
// only from the pass itself; when it did not, the box stayed where it was
// until something else on the page changed, and moved in jumps.
//
// presentedFrame() is the swapchain image as presented (BRO_CAPTURE_PRESENTS=1).

function frames(n) {
    return new Promise((resolve) => {
        const step = () => (--n <= 0 ? resolve() : requestAnimationFrame(step));
        requestAnimationFrame(step);
    });
}

// The box's left edge in the presented frame: the first green pixel on row 60.
function presentedLeft() {
    const img = presentedFrame();
    if (!img) return null;
    const y = 60;
    for (let x = 0; x < img.width; x++) {
        const i = (y * img.width + x) * 4;
        if (img.data[i + 1] > 200 && img.data[i] < 60 && img.data[i + 2] < 60) return x;
    }
    return -1;
}

async function run() {
    // Until the page's first frame is on screen.
    let start = null;
    const settle = Date.now() + 5000;
    do {
        await frames(2);
        start = presentedLeft();
    } while (start !== null && start < 0 && Date.now() < settle);
    if (start === null) {
        assert(false, 'presentedFrame() returned nothing (BRO_CAPTURE_PRESENTS=1 and a readable swapchain needed)');
        return;
    }
    assert(start === 0, `the box starts at the left edge, presented at ${start}`);

    const box = document.getElementById('box');
    box.animate([{ left: '0px' }, { left: '240px' }], { duration: 4000, easing: 'linear', fill: 'forwards' });

    // Nothing else on the page changes; the animation alone must move the box.
    // Sampled by wall time (the offscreen driver does not pace frames to a
    // display): 60 px a second, every 100 ms.
    const seen = [];
    for (let i = 0; i < 12; i++) {
        const until = Date.now() + 100;
        while (Date.now() < until) await frames(1);
        await frames(2);
        seen.push(presentedLeft());
    }
    const moves = seen.filter((x, i) => i > 0 && x > seen[i - 1]).length;
    assert(seen[seen.length - 1] > 20, `the box moved on screen: presented lefts ${seen}`);
    assert(moves >= seen.length - 2, `the box moved between every few frames, not in jumps: ${seen}`);
    console.log('windowed base animation: ' + seen.join(' '));
}

const watchdog = setTimeout(() => {
    assert(false, 'windowed base animation test timed out');
    window.close();
}, 30000);
run().catch((e) => assert(false, 'uncaught: ' + e)).finally(() => {
    clearTimeout(watchdog);
    window.close();
});
