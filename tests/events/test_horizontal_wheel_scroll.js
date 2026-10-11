// Horizontal wheel scrolling and the horizontal scrollbar, following Chromium:
//   - a horizontal wheel delta (tilt wheel, trackpad) scrolls the nearest
//     scroller that overflows horizontally and can still move that way, and
//     chains outward when it is pinned at that edge;
//   - Shift + vertical wheel scrolls horizontally;
//   - a plain vertical wheel never scrolls a box sideways, even one that
//     overflows only horizontally;
//   - an overlay scrollbar sits along the bottom edge by the same rules as the
//     vertical one along the right, and its thumb can be dragged and its track
//     paged.

const root = document.getElementById('root');
const SDLK_LSHIFT = 0x400000E1;

root.innerHTML =
    '<div id="outer" style="position:absolute;left:20px;top:20px;width:300px;height:100px;' +
    'overflow-x:auto;overflow-y:hidden;background:#ffffff;color-scheme:light;">' +
    '  <div id="wide" style="width:2000px;height:60px;background:#0000ff;"></div>' +
    '</div>' +
    // A horizontal scroller nested in another: inner pins, outer takes over.
    '<div id="parent" style="position:absolute;left:20px;top:200px;width:300px;height:100px;overflow-x:auto;">' +
    '  <div style="width:600px;height:90px;">' +
    '    <div id="inner" style="width:200px;height:80px;overflow-x:auto;">' +
    '      <div style="width:400px;height:70px;"></div>' +
    '    </div>' +
    '  </div>' +
    '</div>';
flush();

const outer = document.getElementById('outer');
const r = outer.getBoundingClientRect();
const cx = r.left + 150, cy = r.top + 30;

// ---- the wheel ----------------------------------------------------------------
let wheelDeltaX = null;
outer.addEventListener('wheel', (e) => { wheelDeltaX = e.deltaX; });
let scrolls = 0;
outer.addEventListener('scroll', () => { scrolls++; });

wheel(cx, cy, 0, 1);
const step = outer.scrollLeft;
assert(step > 0, `a rightward wheel tick scrolls right (scrollLeft ${step})`);
assert(wheelDeltaX > 0, `and the wheel event's deltaX is positive, toward the right (got ${wheelDeltaX})`);
assert(scrolls === 1, `one scroll event (got ${scrolls})`);
assert(outer.scrollTop === 0, 'no vertical movement');

wheel(cx, cy, 0, 2);
assert(outer.scrollLeft === step * 3, `two more ticks: 3 steps (got ${outer.scrollLeft}, step ${step})`);
wheel(cx, cy, 0, -1);
assert(outer.scrollLeft === step * 2, `a leftward tick comes back one (got ${outer.scrollLeft})`);

// Clamped at both ends.
wheel(cx, cy, 0, 1000);
assert(outer.scrollLeft === 1700, `clamped at scrollWidth - clientWidth (got ${outer.scrollLeft})`);
wheel(cx, cy, 0, -1000);
assert(outer.scrollLeft === 0, `and at 0 (got ${outer.scrollLeft})`);

// A vertical wheel does not scroll a horizontal-only scroller sideways.
wheel(cx, cy, 1, 0);
assert(outer.scrollLeft === 0, `a vertical wheel leaves scrollLeft alone (got ${outer.scrollLeft})`);

// Shift + vertical wheel scrolls horizontally; wheel-down goes right. On
// macOS the OS turns it horizontal before bro hears it (platform/wheel.h), so
// what reaches bro there is already the horizontal delta, and a vertical one
// that arrives with Shift held is left vertical.
keyDown(SDLK_LSHIFT);
wheelDeltaX = null;
if (process.platform === 'darwin') {
    wheel(cx, cy, 1, 0);
    assert(outer.scrollLeft === 0, `macOS: a vertical delta with Shift held stays vertical (got ${outer.scrollLeft})`);
    wheel(cx, cy, 0, 1);  // what AppKit hands over for shift+wheel-down
} else {
    wheel(cx, cy, 1, 0);
}
keyUp(SDLK_LSHIFT);
assert(outer.scrollLeft === step, `shift+wheel-down scrolls right one step (got ${outer.scrollLeft})`);
assert(wheelDeltaX > 0, `and reaches the page as deltaX (got ${wheelDeltaX})`);
outer.scrollLeft = 0;

// Chaining: the inner scroller takes the delta until it is pinned, then the
// outer one does.
{
    const parent = document.getElementById('parent');
    const inner = document.getElementById('inner');
    const ir = inner.getBoundingClientRect();
    const ix = ir.left + 50, iy = ir.top + 30;
    wheel(ix, iy, 0, 1);
    assert(inner.scrollLeft > 0 && parent.scrollLeft === 0,
           `the inner scroller takes the first tick (inner ${inner.scrollLeft}, parent ${parent.scrollLeft})`);
    wheel(ix, iy, 0, 1000);
    assert(inner.scrollLeft === 200, `inner pinned at its end (got ${inner.scrollLeft})`);
    const parentBefore = parent.scrollLeft;
    const ir2 = inner.getBoundingClientRect();
    wheel(ir2.left + 50, ir2.top + 30, 0, 1);
    assert(parent.scrollLeft > parentBefore,
           `a pinned inner scroller chains to the parent (parent ${parent.scrollLeft})`);
}

// ---- the scrollbar ------------------------------------------------------------
// Overlay bar: 8px thick, 2px in from the bottom edge, thumb = view/content of
// the track (300 * 300/2000 = 45px).
const barY = r.top + r.height - 2 - 4;
const dark = (p) => p.r < 210 && p.g < 210 && p.b < 210;
const white = (p) => p.r > 245 && p.g > 245 && p.b > 245;
flush();
assert(dark(getPixel(r.left + 20, barY)), 'the horizontal thumb is painted at the left of the bottom edge');
assert(!dark(getPixel(r.left + 200, barY)), 'the track past the thumb is not thumb-coloured');
assert(white(getPixel(r.left + 20, r.top + 80)), 'nothing painted above the bar');
assert(white(getPixel(r.left + r.width - 6, r.top + 80)),
       'no vertical bar: the box does not overflow vertically');

outer.scrollLeft = 1700;
flush();
assert(dark(getPixel(r.left + r.width - 20, barY)), 'at the end the thumb sits at the right of the track');
assert(!dark(getPixel(r.left + 20, barY)), 'and has left the start of it');

// Paging: a press on the track before the thumb pages one view back.
outer.scrollLeft = 1700;
mouseDown(r.left + 20, barY);
mouseUp(r.left + 20, barY);
assert(outer.scrollLeft === 1400, `track press pages back by the view width (got ${outer.scrollLeft})`);

// Dragging the thumb: 85px of travel over a 255px thumb range is a third of
// the 1700px scroll range.
outer.scrollLeft = 0;
flush();
mouseDown(r.left + 20, barY);
mouseMove(r.left + 105, barY);
mouseUp(r.left + 105, barY);
assert(Math.abs(outer.scrollLeft - 1700 / 3) < 2,
       `dragging the thumb 85px scrolls a third of the way (got ${outer.scrollLeft})`);

root.innerHTML = '';
