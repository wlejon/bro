// Horizontal scrolling, end to end: scrollLeft / scrollWidth on a box that
// overflows sideways must move what is painted, what getBoundingClientRect
// reports, and what hit testing finds, and must clamp and fire `scroll` — the
// same contract scrollTop has always kept.
//
// Two shapes, because they reach the scroll extent by different routes:
//   - a static wide child, whose margin box is in-flow content;
//   - a position:relative spacer with absolutely positioned cells, the way the
//     kit's virtual lists lay out — the cells reach the extent only as
//     absolutely positioned descendants whose containing block is inside the
//     scroller.

const root = document.getElementById('root');

const near = (a, b, tol = 0.5) => Math.abs(a - b) <= tol;
const isRed = (p) => p.r > 200 && p.g < 60 && p.b < 60;
const isBlue = (p) => p.b > 200 && p.r < 60 && p.g < 60;

function cellsHtml(abs) {
    let html = '';
    for (let i = 0; i < 20; i++) {
        const bg = i === 5 || i === 19 ? '#ff0000' : '#0000ff';
        const place = abs
            ? `position:absolute;left:${i * 100}px;top:0;`
            : 'flex:none;';
        html += `<div id="c${i}" style="${place}width:100px;height:60px;background:${bg};"></div>`;
    }
    return html;
}

function checkScroller(label, scroller, expectScrollWidth) {
    const cell5 = document.getElementById('c5');
    const cell19 = document.getElementById('c19');
    const sr = scroller.getBoundingClientRect();

    assert(scroller.clientWidth === 300, `${label}: clientWidth 300 (got ${scroller.clientWidth})`);
    assert(scroller.scrollWidth === expectScrollWidth,
           `${label}: scrollWidth ${expectScrollWidth} (got ${scroller.scrollWidth})`);
    assert(scroller.scrollLeft === 0, `${label}: starts at scrollLeft 0`);

    const before = cell5.getBoundingClientRect();
    assert(near(before.left, sr.left + 500), `${label}: cell 5 starts 500px in (left ${before.left})`);

    let scrolls = 0;
    const onScroll = () => { scrolls++; };
    scroller.addEventListener('scroll', onScroll);

    scroller.scrollLeft = 500;
    assert(scroller.scrollLeft === 500, `${label}: scrollLeft reads back 500 (got ${scroller.scrollLeft})`);
    assert(scrolls === 1, `${label}: one scroll event (got ${scrolls})`);
    flush();

    const after = cell5.getBoundingClientRect();
    assert(near(after.left, sr.left), `${label}: cell 5 rect moved left by 500 (left ${after.left}, scroller ${sr.left})`);
    assert(near(after.top, before.top), `${label}: and not vertically`);

    const px = sr.left + 50, py = sr.top + 30;
    const hit = document.elementFromPoint(px, py);
    assert(hit === cell5, `${label}: elementFromPoint finds cell 5 (got ${hit && hit.id})`);
    assert(isRed(getPixel(px, py)), `${label}: cell 5's red is painted at the scroller's left edge`);
    assert(isBlue(getPixel(sr.left + 150, py)), `${label}: cell 6's blue right after it`);

    let clicked = null;
    const onClick = (e) => { clicked = e.target; };
    scroller.addEventListener('click', onClick);
    click(px, py);
    assert(clicked === cell5, `${label}: a real click lands on cell 5 (got ${clicked && clicked.id})`);
    scroller.removeEventListener('click', onClick);

    // Clamping: past the end stops at scrollWidth - clientWidth, below 0 at 0.
    const max = expectScrollWidth - 300;
    scroller.scrollLeft = 99999;
    assert(scroller.scrollLeft === max, `${label}: clamped to ${max} (got ${scroller.scrollLeft})`);
    flush();
    const last = cell19.getBoundingClientRect();
    assert(near(last.left, sr.left + 1900 - max),
           `${label}: cell 19 sits where the max offset puts it (left ${last.left})`);

    scrolls = 0;
    scroller.scrollLeft = 99999;
    assert(scrolls === 0, `${label}: no scroll event when the clamped offset did not move`);

    scroller.scrollLeft = -40;
    assert(scroller.scrollLeft === 0, `${label}: clamped at 0 (got ${scroller.scrollLeft})`);

    // scrollTo / scrollBy take the left component too.
    scroller.scrollTo({ left: 300 });
    assert(scroller.scrollLeft === 300, `${label}: scrollTo({left}) (got ${scroller.scrollLeft})`);
    scroller.scrollBy({ left: 50 });
    assert(scroller.scrollLeft === 350, `${label}: scrollBy({left}) (got ${scroller.scrollLeft})`);
    scroller.scrollLeft = 0;

    scroller.removeEventListener('scroll', onScroll);
}

// ---- a static wide child ----------------------------------------------------
root.innerHTML =
    '<div id="scroller" style="position:absolute;left:20px;top:20px;width:300px;height:100px;' +
    'overflow-x:auto;overflow-y:hidden;background:#ffffff;">' +
    '<div id="wide" style="display:flex;width:2000px;height:60px;">' + cellsHtml(false) + '</div>' +
    '</div>';
// No flush: content built in this same turn is laid out by the reads and
// writes themselves, as in a browser.
checkScroller('static wide child', document.getElementById('scroller'), 2000);

// ---- absolutely positioned cells in a relative spacer ------------------------
root.innerHTML =
    '<div id="scroller" style="position:absolute;left:20px;top:20px;width:300px;height:100px;' +
    'overflow:auto;background:#ffffff;">' +
    '<div id="spacer" style="position:relative;width:2000px;height:60px;">' + cellsHtml(true) + '</div>' +
    '</div>';
flush();
checkScroller('abspos cells', document.getElementById('scroller'), 2000);

// A cell placed past the spacer still counts: its containing block (the
// spacer) is inside the scroller.
{
    const scroller = document.getElementById('scroller');
    const far = document.createElement('div');
    far.style.cssText = 'position:absolute;left:2400px;top:0;width:100px;height:60px;';
    document.getElementById('spacer').appendChild(far);
    assert(scroller.scrollWidth === 2500, `an abspos cell past the spacer extends scrollWidth (got ${scroller.scrollWidth})`);
    scroller.scrollLeft = 2200;
    assert(scroller.scrollLeft === 2200, `and can be scrolled to (got ${scroller.scrollLeft})`);
    flush();
    const r = far.getBoundingClientRect();
    assert(near(r.left, scroller.getBoundingClientRect().left + 200), `far cell at 2400 - 2200 (left ${r.left})`);
}

// ---- padding is part of scrollWidth, as scrollHeight's is --------------------
root.innerHTML =
    '<div id="padded" style="position:absolute;left:0;top:0;width:300px;height:50px;' +
    'padding:0 10px;box-sizing:content-box;overflow-x:auto;"><div style="width:1000px;height:20px;"></div></div>';
{
    const padded = document.getElementById('padded');
    assert(padded.clientWidth === 320, `padded clientWidth 320 (got ${padded.clientWidth})`);
    assert(padded.scrollWidth === 1020, `padded scrollWidth = 10 + 1000 + 10 (got ${padded.scrollWidth})`);
    padded.scrollLeft = 5000;
    assert(padded.scrollLeft === 700, `padded max = 1020 - 320 (got ${padded.scrollLeft})`);
}

// ---- only a scroll container scrolls ------------------------------------------
root.innerHTML =
    '<div id="spill" style="position:absolute;left:0;top:0;width:300px;height:50px;">' +
    '<div style="width:1000px;height:20px;"></div></div>' +
    '<div id="ypaired" style="position:absolute;left:0;top:100px;width:300px;height:50px;overflow-y:auto;">' +
    '<div style="width:1000px;height:20px;"></div></div>';
{
    const spill = document.getElementById('spill');
    assert(spill.scrollWidth >= 1000, `a visible-overflow box still reports its scrollWidth (got ${spill.scrollWidth})`);
    spill.scrollLeft = 200;
    assert(spill.scrollLeft === 0, `but overflow:visible does not scroll (got ${spill.scrollLeft})`);

    // overflow-y:auto alone makes overflow-x compute to auto (CSS Overflow 3).
    const ypaired = document.getElementById('ypaired');
    ypaired.scrollLeft = 200;
    assert(ypaired.scrollLeft === 200, `overflow-y:auto alone scrolls horizontally too (got ${ypaired.scrollLeft})`);
}

// ---- scrollIntoView brings a cell into view horizontally ----------------------
root.innerHTML =
    '<div id="scroller" style="position:absolute;left:20px;top:20px;width:300px;height:100px;' +
    'overflow:auto;"><div id="spacer" style="position:relative;width:2000px;height:60px;">' +
    cellsHtml(true) + '</div></div>';
{
    const scroller = document.getElementById('scroller');
    document.getElementById('c12').scrollIntoView({ inline: 'start' });
    assert(scroller.scrollLeft === 1200, `scrollIntoView inline:start (got ${scroller.scrollLeft})`);
    document.getElementById('c2').scrollIntoView();   // inline defaults to nearest
    assert(scroller.scrollLeft === 200, `scrollIntoView nearest from the right (got ${scroller.scrollLeft})`);
}

root.innerHTML = '';
