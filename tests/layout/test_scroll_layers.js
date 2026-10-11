// Scroll layers (layout/scroll_layer.h): a scroll container's content,
// recorded in its own content space and composited where the scroll puts it,
// under the scroller's clip. Every frame here is compared with the same frame
// recorded and rastered whole with no layers at all (setPaintCache(false)),
// on both alternating pools, on a page with nothing animating: a scroller with
// a transparent background over a page that is not one colour, content after
// it that overlaps it, a scroller inside it, scrolls past the layer's window,
// one at a fractional place, a devicePixelRatio of 2; and the scrollers that must stay painted in place
// (positioned, sticky and fixed boxes in them, an effect above them, a rounded
// clip) are, with their reason counted (perf.stats().paint.scrollLayers).

document.body.style.margin = '0';
// A backdrop that is not one colour, under a transparent scroller.
document.body.style.background = 'linear-gradient(90deg, #fee, #eef)';

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
const colours = ['#c33', '#3a3', '#36c', '#cc3', '#3cc'];
function scroller(rows, left, top, extra) {
    const sc = document.createElement('div');
    sc.style.cssText = `position: absolute; left: ${left}px; top: ${top}px; width: 220px; height: 200px; ` +
                       `overflow: auto; border: 2px solid #333; ${extra || ''}`;
    for (let i = 0; i < rows; ++i) {
        const row = document.createElement('div');
        row.style.cssText = `height: 22px; margin: 2px; background: ${colours[i % 5]}`;
        row.textContent = 'row ' + i;
        sc.appendChild(row);
    }
    document.body.appendChild(sc);
    return sc;
}
const layers = () => perf.stats().paint.scrollLayers;

// A plain scroller, transparent over the gradient; a box after it overlaps
// its lower half (it must stay above the layer).
const a = scroller(200, 10, 10);
const over = document.createElement('div');
over.style.cssText = 'position: absolute; left: 60px; top: 150px; width: 300px; height: 40px; background: rgba(0,0,0,0.5)';
document.body.appendChild(over);
// A scroller inside it, painted in its layer.
const inner = document.createElement('div');
inner.style.cssText = 'height: 60px; overflow: auto; margin: 4px; border: 1px solid #000';
for (let i = 0; i < 20; ++i) {
    const r = document.createElement('div');
    r.textContent = 'inner ' + i;
    r.style.background = colours[(i + 2) % 5];
    inner.appendChild(r);
}
a.insertBefore(inner, a.children[5]);
flush();
setPaintCache(true);
frameImage();
frameImage();
const l0 = layers();
check('first frame');
assert(layers().layers > l0.layers, 'the scroller was composited as a layer');
for (const y of [5, 40, 41, 300, 1200, 4000, 3990, 0]) {
    a.scrollTop = y;
    check(`scrolled to ${y}`);
}
// A scroll the layer's window still holds records nothing in the layer, and
// one by whole device pixels rasters nothing either: the compositor moves the
// picture.
a.scrollTop = 100;
check('scrolled to 100');
{
    const r0 = layers();
    for (const y of [104, 150, 230]) {
        a.scrollTop = y;
        check(`scrolled to ${y}, within the window`);
    }
    const r1 = layers();
    assert(r1.reused - r0.reused >= 6, `the layer's recording stood (${r1.reused - r0.reused} reused)`);
    assert(r1.rasters === r0.rasters, `nothing was rastered (${r1.rasters - r0.rasters})`);
}
inner.scrollTop = 30;
check('the inner scroller scrolled');
assert(layers().vetoes.nested > l0.vetoes.nested, 'the inner one is painted in the outer layer (nested)');
a.children[8].style.background = '#000';
check('a row in the layer changed');

// A scroller at a fractional place: its layer is rastered at the sub-pixel
// phase painting in place would have, not resampled to it.
{
    const before = layers().layers;
    const f = scroller(80, 270.5, 230.25);
    f.style.fontSize = '11px';
    // Rows wider than it, cut by its clip at x = 490.5: an aliased clip
    // keeps the pixels whose centres it holds (279 of an edge at 280.5).
    for (const row of f.children) row.style.width = '400px';
    f.scrollTop = 37;
    check('a scroller at a fractional place');
    assert(layers().layers > before, 'the fractional scroller was composited as a layer');
    f.scrollTop = 410;
    check('a scroller at a fractional place, scrolled');
    f.remove();
}

// Scrollers that stay painted in place, each with its reason.
const cases = [
    // Absolute against the body (the scroller is static here): its scroll
    // does not move it.
    ['positioned', (sc) => {
        sc.style.position = 'static';
        sc.style.marginLeft = sc.style.left;
        sc.children[3].style.position = 'absolute';
        sc.children[3].style.left = '600px';
        sc.children[3].style.top = '300px';
    }],
    ['sticky', (sc) => { sc.children[0].style.position = 'sticky'; sc.children[0].style.top = '0'; }],
    ['positioned', (sc) => { sc.children[2].style.position = 'fixed'; sc.children[2].style.top = '0'; }],
    ['effects', (sc) => { sc.style.opacity = '0.9'; }],
    ['roundedClip', (sc) => { sc.style.borderRadius = '8px'; }],
    ['roundedClip', (sc) => {
        // A rounded clip round it, cutting its corner.
        const wrap = document.createElement('div');
        wrap.style.cssText = `position: absolute; left: ${sc.style.left}; top: 0; width: 200px; height: 150px; ` +
                             'overflow: hidden; border-radius: 40px';
        sc.style.left = '0';
        document.body.appendChild(wrap);
        wrap.appendChild(sc);
    }],
];
let left = 260;
for (const [reason, make] of cases) {
    const sc = scroller(60, left, 10);
    make(sc);
    left += 30;
    const before = layers().vetoes[reason];
    sc.scrollTop = 50;
    check(`a scroller with a ${reason} veto`);
    assert(layers().vetoes[reason] > before, `it was painted in place for '${reason}'`);
    sc.scrollTop = 120;
    check(`a scroller with a ${reason} veto, scrolled`);
    sc.remove();
}

// Positioned boxes and stacking contexts its scroll moves are pieces of its
// layer, each where the stacking-context walk paints it: a virtual list (a
// relative spacer, absolute rows, rows taken out and put back as it
// scrolls), a row with a z-index and one with opacity, between boxes outside
// it, positioned before and after it, that overlap it.
{
    const before = document.createElement('div');
    before.style.cssText = 'position: absolute; left: 300px; top: 420px; width: 120px; height: 300px; ' +
                           'background: rgba(200, 0, 120, 0.45)';
    document.body.appendChild(before);
    const vl = document.createElement('div');
    // Off half a device pixel: text crossing a clip edge there is clipped
    // apart from rects painting in place (test_scroll_layers_composited.js).
    vl.style.cssText = 'position: absolute; left: 280.25px; top: 400.25px; width: 240px; height: 260px; ' +
                       'overflow: auto; border: 1px solid #333; font-size: 12px';
    const spacer = document.createElement('div');
    spacer.style.cssText = 'position: relative; height: 6000px';
    vl.appendChild(spacer);
    document.body.appendChild(vl);
    const after = document.createElement('div');
    after.style.cssText = 'position: absolute; left: 470px; top: 500px; width: 120px; height: 90px; ' +
                          'background: rgba(0, 120, 200, 0.5)';
    document.body.appendChild(after);
    const rowH = 30;
    const rows = new Map();
    function render() {
        const top = vl.scrollTop;
        const first = Math.max(0, Math.floor(top / rowH) - 3), last = Math.ceil((top + 260) / rowH) + 3;
        for (const [i, el] of rows) if (i < first || i > last) { el.remove(); rows.delete(i); }
        for (let i = first; i <= last; ++i) {
            if (rows.has(i)) continue;
            const r = document.createElement('div');
            r.style.cssText = `position: absolute; left: 0; right: 0; top: ${i * rowH}px; height: ${rowH - 2}px; ` +
                              `background: ${colours[i % 5]}`;
            if (i % 7 === 3) { r.style.zIndex = '2'; r.style.left = '20px'; }
            if (i % 11 === 5) r.style.opacity = '0.6';
            r.textContent = 'item ' + i;
            spacer.appendChild(r);
            rows.set(i, r);
        }
    }
    render();
    const p0 = layers();
    check('a virtual list');
    assert(layers().pieces > p0.pieces, 'its rows were pieces of its layer');
    for (const y of [10, 13, 100, 700, 2000, 1990, 0]) {
        vl.scrollTop = y;
        render();
        check(`a virtual list scrolled to ${y}`);
    }
    // Scrolled with no rows taken out or put back: the pieces stand.
    vl.scrollTop = 50;
    render();
    check('a virtual list at 50');
    const r0 = layers();
    vl.scrollTop = 52;
    check('a virtual list at 52');
    assert(layers().piecesReused > r0.piecesReused, 'its pieces stood');
    vl.remove();
    before.remove();
    after.remove();
}

// At a devicePixelRatio of 2.
setDeviceScaleFactor(2);
a.scrollTop = 333;
check('dpr 2');
a.scrollTop = 2500;
check('dpr 2, scrolled far');
// At 1.5 a scroll by an odd number of px moves the layer by half a device
// pixel: it is rastered again at that phase (not resampled), and a scroll by
// 2 px (3 device px) is not.
setDeviceScaleFactor(1.5);
a.scrollTop = 2400;
check('dpr 1.5');
{
    const r0 = layers();
    a.scrollTop = 2401;
    check('dpr 1.5, scrolled by half a device pixel');
    const r1 = layers();
    assert(r1.rasters > r0.rasters, 'a scroll by a fraction of a device pixel rasters the layer again');
    a.scrollTop = 2403;
    check('dpr 1.5, scrolled by 3 device pixels');
    assert(layers().rasters === r1.rasters, 'a scroll by whole device pixels does not');
}
setDeviceScaleFactor(1);
check('dpr 1 again');
console.log(`scroll layers: checked, ${layers().layers - l0.layers} layers composited`);
