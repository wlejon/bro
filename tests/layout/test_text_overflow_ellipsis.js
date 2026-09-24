// text-overflow: ellipsis truncates an overflowing line at the box's content
// edge and draws the ellipsis there; `clip` (and overflow: visible) do not.

document.body.style.cssText = 'margin:0;background:#000;';
const box = (id, extra) =>
    '<div id="' + id + '" style="position:absolute;left:20px;width:120px;white-space:nowrap;' +
    'overflow:hidden;font:16px sans-serif;color:#fff;' + extra + '">' +
    'ARDY Motion — text to G1 skeleton motion</div>';
document.body.innerHTML =
    box('ell', 'top:20px;text-overflow:ellipsis;') +
    box('clip', 'top:60px;text-overflow:clip;') +
    '<div id="fits" style="position:absolute;left:20px;top:100px;width:600px;white-space:nowrap;' +
    'overflow:hidden;text-overflow:ellipsis;font:16px sans-serif;color:#fff">short</div>';
flush();

assert(inspect('#ell').indexOf('text-truncated') >= 0, 'the ellipsis box is truncated');
assert(inspect('#clip').indexOf('text-truncated') < 0, 'text-overflow: clip does not truncate');
assert(inspect('#fits').indexOf('text-truncated') < 0, 'text that fits is left alone');

// The two boxes hold the same text; they paint the same up to where the
// ellipsis replaces the tail, and differently in the last ~20px.
function column(top, x) {
    let lit = 0;
    for (let y = top; y < top + 20; y++) {
        const p = getPixel(x, y);
        if (p.r > 96) lit++;
    }
    return lit;
}
let differs = false;
for (let x = 20 + 100; x < 20 + 120; x++) {
    if (column(20, x) !== column(60, x)) { differs = true; break; }
}
assert(differs, 'the ellipsis box paints its tail differently from the clipped one');
let ellInk = 0;
for (let x = 20 + 100; x < 20 + 120; x++) ellInk += column(20, x);
assert(ellInk > 0, 'the ellipsis itself is drawn');

// Widened, the whole text comes back.
document.getElementById('ell').style.width = '800px';
flush();
assert(inspect('#ell').indexOf('text-truncated') < 0, 'widened, nothing is truncated');

document.body.innerHTML = '<div id="root"></div>';
