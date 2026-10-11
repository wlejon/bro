// Compositor-only scrolls (engine/scroll_layers.h): in the windowed frame loop
// (runFrames), a scroll of a scroller composited as a scroll layer that its
// layer's window holds, by whole device pixels, records nothing: the
// compositor moves the layer's picture and its scrollbar thumb. Every
// presented frame (presentedImage) is compared with the same frame recorded
// and rastered whole with no layers (setPaintCache(false)). A scroll the page
// has to record for (past the window, by a fraction of a device pixel, with a
// selection on the page) records, and is right too.

document.body.style.margin = '0';
document.body.style.background = 'linear-gradient(90deg, #fee, #eef)';
const sheet = document.createElement('style');
sheet.textContent = '::selection { background: rgba(0, 0, 255, 0.4) }';
document.head.appendChild(sheet);

function diffCount(a, b) {
    if (!a || !b || a.width !== b.width || a.height !== b.height) return -1;
    let n = 0;
    for (let i = 0; i < a.data.length; i += 4)
        if (Math.abs(a.data[i] - b.data[i]) > 16 || Math.abs(a.data[i + 1] - b.data[i + 1]) > 16 ||
            Math.abs(a.data[i + 2] - b.data[i + 2]) > 16)
            ++n;
    return n;
}
// presentedImage() is null when a capture (the full record below) came after
// the loop's last present: each check needs a frame presented since the last.
function checkPresented(what) {
    const shown = presentedImage();
    assert(shown, `${what}: the loop presented a frame since the last check`);
    setPaintCache(false);
    const full = frameImage();
    setPaintCache(true);
    const n = diffCount(shown, full);
    assert(n === 0, `${what}: the presented frame has ${n} pixels apart from a full record without layers`);
}
const records = () => perf.stats().paint.records;
// Run the windowed loop until what it shows is current: until a run of
// frames neither records nor rasters. runFrames returns with the raster idle
// and its result taken, so the frames after it composite every raster there
// was. A frame count would do only on a fast raster thread: the first raster
// of this page can land after many frames were presented. Returns how many
// frames were presented.
function settle(what) {
    let presented = 0;
    for (let i = 0; i < 200; ++i) {
        const s0 = perf.stats().paint;
        presented += runFrames(2, { stepMs: 16 }).presented;
        const s1 = perf.stats().paint;
        if (s1.records === s0.records && s1.rasterPasses === s0.rasterPasses) return presented;
    }
    assert(false, `${what}: the frame loop settled`);
    return presented;
}
// Scroll and settle; whether the base was recorded for it.
function scrollTo(el, y, what, expectRecord) {
    settle(`${what} (before)`);
    const r0 = records();
    el.scrollTop = y;
    const presented = settle(what);
    const recorded = records() - r0;
    assert(presented >= 1, `${what}: a frame was presented`);
    if (expectRecord) assert(recorded > 0, `${what}: the page was recorded`);
    else assert(recorded === 0, `${what}: nothing was recorded (${recorded} records)`);
    checkPresented(what);
}

const colours = ['#c33', '#3a3', '#36c', '#cc3', '#3cc'];
const sc = document.createElement('div');
// At a fractional place, transparent over the gradient, both bars showing.
// Not on half a device pixel: a clip edge there is one Ganesh keeps a rect's
// pixel at and a glyph's not, painting in place, so no picture clipped once
// matches both (test_scroll_layers.js has a scroller at 270.5 whose text
// stays off that edge).
sc.style.cssText = 'position: absolute; left: 20.25px; top: 30.25px; width: 260px; height: 220px; ' +
                   'overflow: auto; border: 2px solid #333; font-size: 12px';
for (let i = 0; i < 200; ++i) {
    const row = document.createElement('div');
    row.style.cssText = `height: 22px; width: 400px; margin: 2px; background: ${colours[i % 5]}`;
    row.textContent = 'row ' + i;
    sc.appendChild(row);
}
document.body.appendChild(sc);
// A box after it, over its lower half.
const over = document.createElement('div');
over.style.cssText = 'position: absolute; left: 60px; top: 180px; width: 300px; height: 40px; background: rgba(0,0,0,0.5)';
document.body.appendChild(over);
const p = document.createElement('p');
p.textContent = 'Some text outside the scroller.';
p.style.cssText = 'position: absolute; left: 320px; top: 40px';
document.body.appendChild(p);

setPaintCache(true);
settle('the first frame');
assert(perf.stats().paint.scrollLayers.layers > 0, 'the scroller was composited as a layer');
checkPresented('first frame');

for (const y of [40, 41, 120, 237]) scrollTo(sc, y, `scrolled to ${y}`, false);
sc.scrollLeft = 30;
settle('scrolled left by 30');
checkPresented('scrolled left by 30');

// The wheel, as a window gets it: a notch up from 237, which the window
// recorded at scroll 0 (its view and 256 px round it) holds whatever a
// notch is on this platform (3 lines: 48 px on Windows).
{
    settle('the wheel (before)');
    const r0 = records();
    const top0 = sc.scrollTop;
    wheel(100, 100, -1);
    settle('a wheel scroll');
    assert(sc.scrollTop < top0 && sc.scrollTop > 0, `the wheel scrolled up (${top0} -> ${sc.scrollTop})`);
    assert(records() === r0, `a wheel scroll the window holds records nothing (${top0} -> ${sc.scrollTop})`);
    checkPresented('a wheel scroll');
}

// Past the window: the layer is recorded again, then compositor-only again.
scrollTo(sc, 2000, 'scrolled past the window', true);
scrollTo(sc, 2050, 'then within it', false);

// A selection on the page (dragged over the text outside the scroller): the
// base paints selection highlights where the scrolls put them, so with one
// a scroll records.
{
    mouseMove(322, 50);
    mouseDown(322, 50);
    mouseMove(400, 50);
    mouseMove(480, 52);
    mouseUp(480, 52);
    settle('a selection');
    assert(getSelection().toString().length > 0, 'the drag selected text');
    checkPresented('a selection');
    scrollTo(sc, 2060, 'scrolled with a selection', true);
    getSelection().removeAllRanges();
    settle('the selection gone');
    scrollTo(sc, 2070, 'the selection gone', false);
}

// A row changed, then scrolled: the change is recorded, then the scroll is not.
sc.children[96].style.background = '#000';
settle('a row changed');
checkPresented('a row changed');
scrollTo(sc, 2080, 'scrolled after the change', false);

// At devicePixelRatio 1.5 a scroll by 1 px is half a device pixel: recorded
// (the layer is rastered at its new phase); by 2 px it is not.
setDeviceScaleFactor(1.5);
settle('dpr 1.5');
checkPresented('dpr 1.5');
scrollTo(sc, 2081, 'dpr 1.5, by half a device pixel', true);
scrollTo(sc, 2083, 'dpr 1.5, by 3 device pixels', false);
setDeviceScaleFactor(1);
settle('dpr 1 again');
checkPresented('dpr 1 again');

// A virtual list, as helm's VirtualList is: a relative spacer, absolute rows
// put in and taken out on 'scroll'. Its rows are pieces of its layer; a
// scroll that keeps the same rows is the compositor's, one that brings rows
// in records them.
{
    sc.remove();
    const vl = document.createElement('div');
    vl.style.cssText = 'position: absolute; left: 20.25px; top: 30.25px; width: 260px; height: 220px; ' +
                       'overflow: auto; border: 1px solid #333; font-size: 12px';
    const spacer = document.createElement('div');
    spacer.style.cssText = 'position: relative; height: 9000px';
    vl.appendChild(spacer);
    document.body.insertBefore(vl, over);
    const rowH = 30;
    const rows = new Map();
    const render = () => {
        const top = vl.scrollTop;
        const first = Math.max(0, Math.floor(top / rowH) - 3), last = Math.ceil((top + 220) / rowH) + 3;
        for (const [i, el] of rows) if (i < first || i > last) { el.remove(); rows.delete(i); }
        for (let i = first; i <= last; ++i) {
            if (rows.has(i)) continue;
            const r = document.createElement('div');
            r.style.cssText = `position: absolute; left: 0; right: 0; top: ${i * rowH}px; height: ${rowH - 2}px; ` +
                              `background: ${colours[i % 5]}`;
            r.textContent = 'item ' + i;
            spacer.appendChild(r);
            rows.set(i, r);
        }
    };
    vl.addEventListener('scroll', render);
    render();
    settle('a virtual list');
    checkPresented('a virtual list');
    assert(perf.stats().paint.scrollLayers.pieces > 0, 'its rows are pieces of its layer');
    scrollTo(vl, 12, 'a virtual list, the same rows', false);
    scrollTo(vl, 140, 'a virtual list, rows put in', true);
    scrollTo(vl, 134, 'a virtual list, the same rows again', false);  // rows 1..15, as at 140
}
console.log('compositor-only scrolls: checked');
