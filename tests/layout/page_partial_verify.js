// Driven by tests/layout/test_page_partial_verify.js (in its own process, and
// in a child bro-headless --no-gpu): the page's own partial raster in the
// frame loop, each pass checked against the same recording rastered whole
// (setScrollLayerVerify), on a page of rounded cells with a rounded clip over
// a gradient and text every twenty rows, one cell toggled a frame.

setScrollLayerVerify(true);
const style = document.createElement('style');
style.textContent = `body { margin: 0; background: #fff }
.cell { display: inline-block; width: 6.5px; height: 6px; margin: 1px 1.25px; border-radius: 2px; background: rgb(200,200,200); }
.cell.on { background-color: rgb(9, 9, 9); }
.box { margin: 3.5px; padding: 2px; border-radius: 9px; overflow: hidden; background: linear-gradient(90deg, #f80, #80f); }
.box span { display: block; height: 30px; background: #036; color: #fff }`;
document.head.appendChild(style);
const cells = [];
for (let r = 0; r < 200; r++) {
    const row = document.createElement('div');
    if (r % 20 === 0) {
        const b = document.createElement('div');
        b.className = 'box';
        const s = document.createElement('span');
        s.textContent = 'rounded ' + r;
        b.append(s);
        row.append(b);
    }
    for (let c = 0; c < 19; c++) {
        const s = document.createElement('span');
        s.className = 'cell';
        row.append(s);
        cells.push(s);
    }
    document.body.append(row);
}
flush();
let i = 0;
function tick() {
    cells[(i++ * 37) % cells.length].classList.toggle('on');
    requestAnimationFrame(tick);
}
requestAnimationFrame(tick);
runFrames(30, { stepMs: 16 });
const p0 = perf.stats().paint;
runFrames(200, { stepMs: 16 });
const p1 = perf.stats().paint;
const partial = p1.pagePartialRasters - p0.pagePartialRasters;
const apart = p1.pagePartialMismatch - p0.pagePartialMismatch;
const whole = (k) => p1[k] - p0[k];
console.log(`PAGE_PARTIAL partial=${partial} apart=${apart} of ${p1.rasterPasses - p0.rasterPasses} passes; ` +
            `whole: history=${whole('pageWholeHistory')} cover=${whole('pageWholeCover')} area=${whole('pageWholeArea')}`);
assert(partial >= 20, `a cell toggled draws only what reaches it (${partial} partial passes)`);
assert(apart === 0, `every partial pass of the page is its whole raster, pixel for pixel (${apart} apart)`);
