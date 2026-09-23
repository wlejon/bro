// Boxes a line-clamp cuts away are not painted: no background, no border.
//
// htmlayout's line-clamp keeps the lines up to the clamp point, ends the
// container's height there, and flags every element box past it
// `clampHidden` (their text runs are already emptied). The boxes still have
// geometry — they overflow the container — so the painter has to skip them
// itself, or their backgrounds and borders show below the clamped text.

document.body.style.cssText = 'margin:0;background:#000;';
document.body.innerHTML =
    '<div id="box" style="position:absolute;left:20px;top:20px;width:200px;' +
    'font:16px/20px sans-serif;color:#fff;line-clamp:1;">' +
    '<div id="p1" style="height:20px;background:#00c000;">one</div>' +
    '<div id="p2" style="height:20px;background:#ff0000;">two</div>' +
    '<div id="p3" style="height:20px;border:4px solid #0000ff;box-sizing:border-box;">three</div>' +
    '</div>';
flush();

const box = document.getElementById('box').getBoundingClientRect();
assert(Math.round(box.height) === 20,
       'the container ends after the clamped line, height ' + box.height);

const p2 = document.getElementById('p2').getBoundingClientRect();
const p3 = document.getElementById('p3').getBoundingClientRect();
assert(p2.height > 0 && p2.top >= box.bottom - 0.5,
       'the clamped block still has geometry below the container');

function px(x, y) { return getPixel(Math.round(x), Math.round(y)); }
function isBlack(p) { return p.r < 16 && p.g < 16 && p.b < 16; }

// The kept line's block paints its background (sample its right edge, clear
// of the glyphs).
const kept = px(box.left + 190, box.top + 10);
assert(kept.g > 150 && kept.r < 60 && kept.b < 60,
       'kept block background is painted, got rgb(' + kept.r + ',' + kept.g + ',' + kept.b + ')');

// The clamped block's background is not.
const bg2 = px(p2.left + 190, p2.top + 10);
assert(isBlack(bg2),
       'clamped block background is not painted, got rgb(' + bg2.r + ',' + bg2.g + ',' + bg2.b + ')');

// Nor is the clamped block's border.
const bd3 = px(p3.left + 100, p3.top + 1);
assert(isBlack(bd3),
       'clamped block border is not painted, got rgb(' + bd3.r + ',' + bd3.g + ',' + bd3.b + ')');

// Raising the clamp brings the second block back, background and all.
document.getElementById('box').style.lineClamp = '2';
flush();
const bg2b = px(document.getElementById('p2').getBoundingClientRect().left + 190,
                document.getElementById('p2').getBoundingClientRect().top + 10);
assert(bg2b.r > 150 && bg2b.g < 60,
       'unclamped block background is painted again, got rgb(' + bg2b.r + ',' + bg2b.g + ',' + bg2b.b + ')');
