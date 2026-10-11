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
    f.scrollTop = 37;
    check('a scroller at a fractional place');
    assert(layers().layers > before, 'the fractional scroller was composited as a layer');
    f.scrollTop = 410;
    check('a scroller at a fractional place, scrolled');
    f.remove();
}

// Scrollers that stay painted in place, each with its reason.
const cases = [
    ['positioned', (sc) => { sc.children[3].style.position = 'relative'; sc.children[3].style.left = '10px'; }],
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

// At a devicePixelRatio of 2.
setDeviceScaleFactor(2);
a.scrollTop = 333;
check('dpr 2');
a.scrollTop = 2500;
check('dpr 2, scrolled far');
setDeviceScaleFactor(1);
check('dpr 1 again');
console.log(`scroll layers: checked, ${layers().layers - l0.layers} layers composited`);
