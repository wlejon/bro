// A ring with per-side border colours is a circle, not a square in a circle.
//
// `border-radius: 50%` plus one differing `border-*-color` is the whole of the
// CSS loading-spinner idiom, and the engine draws it by stroking the full
// rounded rect once per side, clipped to that side's wedge. The wedge used to be
// the flat outer-corner→inner-corner trapezoid, which is right for a square box
// and badly wrong for a round one: a corner's split line stays inside the
// corner's rx×ry box for a full RADIUS, while the trapezoid is only one
// border-width tall. Everything the side owned of its two arcs fell outside the
// clip and went unpainted — four disconnected chords with the corners missing,
// and a spinner reduced to a sliver near the top tangent.
//
// The points sampled below are the ones that distinguish the two: 30° off each
// axis is inside that side's share of the arc, so it must carry that side's
// colour, and it lies a radius deep rather than a border-width deep.

const R = 40;          // outer radius: an 80x80 box
const BW = 8;          // border width on every side
const MID = R - BW / 2;  // radius of the ring's midline

document.body.style.cssText = 'margin:0;background:#000;';
document.body.innerHTML =
    '<div id="ring" style="position:absolute;left:100px;top:60px;' +
    'box-sizing:border-box;width:' + (2 * R) + 'px;height:' + (2 * R) + 'px;' +
    'border:' + BW + 'px solid;border-radius:50%;' +
    'border-top-color:#ff8000;border-right-color:#ff0000;' +
    'border-bottom-color:#00c000;border-left-color:#0000ff;"></div>' +
    // Same per-side colours, near-square corners: the k==1 case, where the
    // wedge must stay the plain trapezoid it always was.
    '<div id="boxy" style="position:absolute;left:300px;top:60px;' +
    'box-sizing:border-box;width:' + (2 * R) + 'px;height:' + (2 * R) + 'px;' +
    'border:' + BW + 'px solid;border-radius:2px;' +
    'border-top-color:#ff8000;border-right-color:#ff0000;' +
    'border-bottom-color:#00c000;border-left-color:#0000ff;"></div>';
flush();

const ring = document.getElementById('ring').getBoundingClientRect();
assert(Math.round(ring.width) === 2 * R && Math.round(ring.height) === 2 * R,
       'ring border box is ' + (2 * R) + 'x' + (2 * R) + ', got ' +
       ring.width + 'x' + ring.height);

// Classify a sample by which of the four side colours it is nearest, so
// antialiasing along the ring edge does not decide the test. `bg` is the
// unpainted page.
function nameOf(p) {
    const cands = [
        ['top',    255, 128, 0],
        ['right',  255, 0,   0],
        ['bottom', 0,   192, 0],
        ['left',   0,   0,   255],
        ['bg',     0,   0,   0],
    ];
    let best = null, bestD = Infinity;
    for (const [nm, r, g, b] of cands) {
        const d = (p.r - r) * (p.r - r) + (p.g - g) * (p.g - g) + (p.b - b) * (p.b - b);
        if (d < bestD) { bestD = d; best = nm; }
    }
    return best;
}

// Sample the ring midline at `deg` measured counter-clockwise from +x, screen
// axes (so 90° is the TOP of the box).
function atAngle(rect, deg, radius) {
    const cx = rect.left + rect.width / 2, cy = rect.top + rect.height / 2;
    const a = deg * Math.PI / 180;
    return getPixel(Math.round(cx + radius * Math.cos(a)),
                    Math.round(cy - radius * Math.sin(a)));
}

// Two samples per side, 30° either side of that side's axis. Both are well
// inside the side's own quadrant (the splits are at 45/135/225/315°) and both
// sit a radius deep into the corner rather than a border-width deep, which is
// exactly what the old flat trapezoid clipped away.
const cases = [
    [ 90, 'top'],    [ 60, 'top'],    [120, 'top'],
    [  0, 'right'],  [-30, 'right'],  [ 30, 'right'],
    [270, 'bottom'], [240, 'bottom'], [300, 'bottom'],
    [180, 'left'],   [150, 'left'],   [210, 'left'],
];
for (const [deg, want] of cases) {
    const px = atAngle(ring, deg, MID);
    const got = nameOf(px);
    assert(got === want,
           'ring at ' + deg + '° is the ' + want + ' colour, got ' + got +
           ' rgb(' + px.r + ',' + px.g + ',' + px.b + ')');
}

// It is a RING, not a disc: the middle is unpainted.
assert(nameOf(atAngle(ring, 0, 0)) === 'bg', 'ring centre is unpainted');

// It is a CIRCLE, not a rounded square: the box's own corner sits outside the
// outer arc, so nothing paints there. (r = R*sqrt(2) - 3 is just inside the
// corner, still well outside the arc.)
const cornerR = R * Math.SQRT2 - 3;
for (const deg of [45, 135, 225, 315]) {
    const got = nameOf(atAngle(ring, deg, cornerR));
    assert(got === 'bg', 'ring corner at ' + deg + '° is unpainted, got ' + got);
}

// Adjacent sides meet ON the diagonal and nowhere else: 1px inside the split
// line the pixel already belongs to one side, never to the other's colour.
// (Sampled off the midline radius so AA at the arc edges is not in play.)
const splits = [[135, 'top', 'left'], [45, 'top', 'right'],
                [225, 'bottom', 'left'], [315, 'bottom', 'right']];
for (const [deg, a, b] of splits) {
    const na = nameOf(atAngle(ring, deg - 6, MID));
    const nb = nameOf(atAngle(ring, deg + 6, MID));
    assert(na !== nb, 'the ' + a + '/' + b + ' split at ' + deg + '° separates ' +
           'two colours, both read ' + na);
    assert((na === a || na === b) && (nb === a || nb === b),
           'either side of the ' + deg + '° split is ' + a + ' or ' + b +
           ', got ' + na + '/' + nb);
}

// The near-square box: the trapezoid wedge is still correct, i.e. each side's
// straight run carries its own colour and the diagonal still splits the corner.
const boxy = document.getElementById('boxy').getBoundingClientRect();
function boxPixel(dx, dy) {
    return nameOf(getPixel(Math.round(boxy.left + dx), Math.round(boxy.top + dy)));
}
assert(boxPixel(R, BW / 2) === 'top', 'square-cornered top edge is the top colour');
assert(boxPixel(2 * R - BW / 2, R) === 'right', 'square-cornered right edge is the right colour');
assert(boxPixel(R, 2 * R - BW / 2) === 'bottom', 'square-cornered bottom edge is the bottom colour');
assert(boxPixel(BW / 2, R) === 'left', 'square-cornered left edge is the left colour');
// Inside the top-left corner, above the 45° diagonal is top, below it is left.
assert(boxPixel(BW - 2, 2) === 'top', 'above the corner diagonal is the top colour');
assert(boxPixel(2, BW - 2) === 'left', 'below the corner diagonal is the left colour');

console.log('PASS border-radius per-side colours');
