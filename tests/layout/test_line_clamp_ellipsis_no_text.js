// A line-clamp container whose lines hold no text still shows its ellipsis.
//
// With no text node kept to carry it (lines of inline-blocks), htmlayout's
// line-clamp puts the ellipsis on the container itself: a text run in the
// container's content coordinates and font, marked kContainerEllipsisSrc.
// The painter draws it after the children, at the container's content origin.

document.body.style.cssText = 'margin:0;background:#000;';
const block = (c) => '<span style="display:inline-block;width:40px;height:24px;' +
    'vertical-align:top;background:' + c + ';"></span>';
// No whitespace between the blocks: whitespace would be a text node.
document.body.innerHTML =
    '<div id="box" style="position:absolute;left:20px;top:20px;width:110px;' +
    'font:20px/24px sans-serif;color:#fff;line-clamp:1;">' +
    block('#008000') + block('#008000') + block('#800000') + block('#800000') +
    '</div>';
flush();

const box = document.getElementById('box').getBoundingClientRect();
assert(Math.round(box.height) === 24,
       'the container ends after the first line, height ' + box.height);

function brightIn(x0, y0, x1, y1) {
    let n = 0;
    for (let y = Math.round(y0); y < Math.round(y1); y++) {
        for (let x = Math.round(x0); x < Math.round(x1); x++) {
            const p = getPixel(x, y);
            if (p.r > 128 && p.g > 128 && p.b > 128) n++;
        }
    }
    return n;
}

// The first line keeps two 40px blocks; the ellipsis sits after them, in the
// 30px left on the line, drawn in the container's white.
const lit = brightIn(box.left + 80, box.top, box.left + 110, box.top + 24);
assert(lit > 0, 'the ellipsis is painted after the kept blocks, lit pixels ' + lit);

// The kept blocks themselves are untouched by it.
const g = getPixel(Math.round(box.left + 20), Math.round(box.top + 12));
assert(g.g > 100 && g.r < 40, 'the first kept block paints, got rgb(' + g.r + ',' + g.g + ',' + g.b + ')');

// Unclamped, the container draws no ellipsis anywhere on its first line.
document.getElementById('box').style.lineClamp = 'none';
flush();
const litAfter = brightIn(box.left + 80, box.top, box.left + 110, box.top + 24);
assert(litAfter === 0, 'no ellipsis once the clamp is lifted, lit pixels ' + litAfter);

console.log('test_line_clamp_ellipsis_no_text: OK');
