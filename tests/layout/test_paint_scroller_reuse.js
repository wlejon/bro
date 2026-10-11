// Scrolling an element moves the boxes in it and nothing else, so the paint
// cache replays their spans under a translate (layout/paint_cache.h:
// SpanKey::fitsIn) — unless the scroll brought into view what the recording
// culled: boxes more than kCullSlack (256 px) outside the scroller's clip,
// which a long list holds. Each step scrolls (a little, a lot, back, a scroller
// inside a scroller) and compares the frame the cache made with the same
// frame recorded and rastered whole (setPaintCache(false)), on both
// alternating pools, on a page with nothing animating.

document.body.style.margin = '0';
document.body.style.background = '#fff';

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
        assert(n === 0, `${what}: capture ${k + 1} has ${n} pixels apart from a full re-record`);
    }
}

const colours = ['#c33', '#3a3', '#36c', '#cc3', '#3cc'];
function list(rows, height) {
    const sc = document.createElement('div');
    sc.style.cssText = `height: ${height}px; width: 300px; overflow: auto; margin: 10px; border: 1px solid #000`;
    for (let i = 0; i < rows; ++i) {
        const row = document.createElement('div');
        row.style.cssText = `height: 24px; margin: 2px; background: ${colours[i % 5]}; border-radius: 3px`;
        const label = document.createElement('span');
        label.textContent = 'row ' + i;
        row.appendChild(label);
        sc.appendChild(row);
    }
    return sc;
}
const outer = list(400, 300);
document.body.appendChild(outer);
// A scroller inside the scroller.
const inner = list(60, 120);
outer.insertBefore(inner, outer.children[20]);
flush();

setPaintCache(true);
frameImage();
frameImage();
const before = perf.stats().paint;
for (const y of [4, 8, 60, 61, 300, 302, 2000, 1990, 0, 5000, 4990]) {
    outer.scrollTop = y;
    check(`outer scrolled to ${y}`);
}
outer.scrollTop = 400;
for (const y of [3, 40, 700, 0]) {
    inner.scrollTop = y;
    check(`inner scrolled to ${y}`);
}
const after = perf.stats().paint;
const translated = after.reusedScrolled - before.reusedScrolled;
assert(translated > 50, `rows were reused under a translate as they scrolled (${translated})`);
console.log(`paint scroller reuse: ${translated} spans translated, ${after.recorded - before.recorded} recorded`);
