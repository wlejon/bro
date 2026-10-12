// Driver for tests/layout/test_scroll_layer_partial_cpu.js, run in a child
// bro-headless --no-gpu: a scroll layer on CPU surfaces whose rows change on
// hover. The pointer moving over its rows changes one row (and the one it
// left) each time: the layer is rastered in place, only what changed drawn
// again (ScrollLayerSet::rasterPart), and every frame matches the same frame
// recorded and rastered whole without layers.

document.body.style.margin = '0';
document.body.style.background = 'linear-gradient(90deg, #fee, #eef)';
const sheet = document.createElement('style');
sheet.textContent = '.row { height: 24px; margin: 2px; font-size: 13px } .row:hover { background: #222; color: #fff }';
document.head.appendChild(sheet);
const colours = ['#c33', '#3a3', '#36c', '#cc3', '#3cc'];
const sc = document.createElement('div');
sc.style.cssText = 'position: absolute; left: 20.5px; top: 30.5px; width: 300px; height: 400px; overflow: auto; ' +
                   'border: 1px solid #333';
for (let i = 0; i < 300; ++i) {
    const row = document.createElement('div');
    row.className = 'row';
    row.style.background = colours[i % 5];
    row.textContent = 'row ' + i;
    sc.appendChild(row);
}
document.body.appendChild(sc);

function diffCount(a, b) {
    if (!a || !b || a.width !== b.width || a.height !== b.height) return -1;
    let n = 0;
    for (let i = 0; i < a.data.length; i += 4)
        if (Math.abs(a.data[i] - b.data[i]) > 16 || Math.abs(a.data[i + 1] - b.data[i + 1]) > 16 ||
            Math.abs(a.data[i + 2] - b.data[i + 2]) > 16)
            ++n;
    return n;
}
function check(what) {
    flush();
    const cached = [frameImage(), frameImage()];
    setPaintCache(false);
    const full = frameImage();
    setPaintCache(true);
    for (let k = 0; k < 2; ++k) {
        const n = diffCount(cached[k], full);
        assert(n === 0, `${what}: capture ${k + 1} has ${n} pixels apart from a full record without layers`);
    }
}
const layers = () => perf.stats().paint.scrollLayers;

// Each partial raster is checked against the same recording rastered whole,
// pixel for pixel.
setScrollLayerVerify(true);
setPaintCache(true);
flush();
frameImage();
frameImage();
assert(layers().layers > 0, 'the list is a scroll layer');
check('first frame');
const p0 = layers();
for (const y of [60, 90, 120, 330, 300]) {
    mouseMove(150, y);
    check(`the pointer at ${y}`);
}
sc.scrollTop = 500;
check('scrolled');
mouseMove(150, 200);
check('the pointer on a row after the scroll');
const p1 = layers();
console.log(`PARTIAL_CPU partial=${p1.partialRasters - p0.partialRasters} whole=${p1.rasters - p0.rasters}`);
assert(p1.partialRasters - p0.partialRasters >= 5,
       `a hover that changes a row rasters the layer in place (${p1.partialRasters - p0.partialRasters} partial, ` +
       `${p1.rasters - p0.rasters} whole)`);
assert(p1.partialMismatch === 0, `every partial raster is the whole one, pixel for pixel (${p1.partialMismatch} apart)`);
