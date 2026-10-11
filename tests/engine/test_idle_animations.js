// Animations a page starts or ends while it is idle reach the screen, in the
// windowed frame loop (runFrames) with no input and no requestAnimationFrame.
//
// A tooltip: a timer (the helmapps kit's 550 ms one) appends a tip with a CSS
// fade-in and measures it to place it. The measure lays the document out on
// the main thread (Engine::flushLayoutForRead), which starts the fade-in; it
// started it at the time of the layout thread's last pass, long before on an
// idle page, so the 120 ms fade-in finished at its first tick and the frame
// recorded with it at opacity 0 stayed on screen until the next input — the
// tip showed only as the pointer left its button.
//
// A colour animation that ends: its last tick marks the element so its value
// settles back to the base, and the layout pass cleared that mark with the
// document's dirty flag, so an idle page kept the last animated colour.

document.body.style.margin = '0';
document.body.style.background = '#000';
const sheet = document.createElement('style');
sheet.textContent = `
@keyframes fade-in { from { opacity: 0; } to { opacity: 1; } }
@keyframes tint { from { background-color: #0000ff; } to { background-color: #00ff00; } }
.tip { position: fixed; left: 40px; top: 40px; width: 80px; height: 20px; background: #ff0000;
       animation: fade-in 120ms ease; }
.box { position: absolute; left: 200px; top: 40px; width: 80px; height: 80px; background: #ff0000; }
.box.tinted { animation: tint 150ms linear; }`;
document.head.appendChild(sheet);
const box = document.createElement('div');
box.className = 'box';
document.body.appendChild(box);

// Run the loop until a run of frames neither records nor rasters: what it
// shows is then current (runFrames returns with the raster idle and taken).
function settle(what) {
    for (let i = 0; i < 200; ++i) {
        const s0 = perf.stats().paint;
        runFrames(2, { stepMs: 16 });
        const s1 = perf.stats().paint;
        if (s1.records === s0.records && s1.rasterPasses === s0.rasterPasses) return;
    }
    assert(false, `${what}: the frame loop settled`);
}
function presentedAt(x, y) {
    const img = presentedImage();
    if (!img) return null;
    const i = (Math.round(y * devicePixelRatio) * img.width + Math.round(x * devicePixelRatio)) * 4;
    return [img.data[i], img.data[i + 1], img.data[i + 2]];
}

settle('the page');

// The tooltip: the page idles for a while first, so the layout thread's last
// pass is well in the past when the timer fires.
let tip = null;
setTimeout(() => {
    tip = document.createElement('div');
    tip.className = 'tip';
    document.body.appendChild(tip);
    const r = tip.getBoundingClientRect();  // to place it, as the kit does
    assert(r.width === 80, `the tip was laid out to be measured (${r.width})`);
    // The measure started its fade-in now, not at the last layout pass.
    const anims = tip.getAnimations();
    assert(anims.length === 1 && anims[0].currentTime !== null && anims[0].currentTime < 20,
           `the tip's fade-in starts as it is put in (currentTime ${anims.length ? anims[0].currentTime : 'none'})`);
}, 550);
runFrames(60, { stepMs: 16 });  // idle: the timer fires about a third of the way
settle('the tip');
assert(tip, 'the timer appended the tip');
{
    const px = presentedAt(80, 50);
    assert(px && px[0] > 240 && px[2] < 16, `the tip reached the screen with no input after it (got ${px})`);
}

// The colour animation: it runs out while nothing else happens.
box.classList.add('tinted');
runFrames(40, { stepMs: 16 });  // 640 ms: long past its 150 ms
settle('the colour animation');
{
    const px = presentedAt(240, 80);
    assert(px && px[0] > 240 && px[1] < 16 && px[2] < 16,
           `the box went back to its own colour when its animation ended (got ${px})`);
}
console.log('idle animations: the tip showed and the colour settled');
