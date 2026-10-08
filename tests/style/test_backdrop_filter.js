// backdrop-filter: blur() filters what is painted behind the element, inside
// its border box (rounded by border-radius), under its own background.
// Before, the property was parsed but never painted: glass panels showed
// their backdrop unblurred.

document.body.style.cssText = 'margin:0;background:rgb(255,255,255)';

function show(glassStyle) {
    document.body.innerHTML =
        // A hard black/white edge at x = 200 behind the glass.
        '<div style="position:absolute;left:0;top:0;width:200px;height:300px;background:rgb(0,0,0)"></div>' +
        `<div id="glass" style="position:absolute;left:100px;top:50px;width:200px;height:200px;${glassStyle}"></div>`;
    for (let i = 0; i < 2; ++i) { advanceTime(16); flush(); }
    return getPixels(0, 0, 400, 300);
}
const px = (img, x, y) => img.data[(y * img.width + x) * 4];

// Without a backdrop-filter the edge is sharp everywhere.
let img = show('');
assert(px(img, 196, 150) < 8 && px(img, 204, 150) > 247, 'reference: the edge is sharp');

img = show('backdrop-filter: blur(8px)');
// Inside the glass the edge is blurred: a few px either side are grey.
const l = px(img, 196, 150), r = px(img, 204, 150), mid = px(img, 200, 150);
assert(l > 40 && l < 200, `blurred: just left of the edge is grey, got ${l}`);
assert(r > 55 && r < 215, `blurred: just right of the edge is grey, got ${r}`);
assert(mid > 90 && mid < 170, `blurred: on the edge is mid-grey, got ${mid}`);
// Outside the glass (above and below it) the edge stays sharp.
assert(px(img, 196, 20) < 8 && px(img, 204, 20) > 247, 'outside the glass, above: sharp');
assert(px(img, 196, 280) < 8 && px(img, 204, 280) > 247, 'outside the glass, below: sharp');

// The element's own background paints over the filtered backdrop.
img = show('backdrop-filter: blur(8px); background: rgb(255 0 0 / 0.5)');
{
    const i = (150 * img.width + 200) * 4;
    const [rr, gg, bb] = [img.data[i], img.data[i + 1], img.data[i + 2]];
    assert(rr > 170 && gg > 30 && gg < 110 && Math.abs(gg - bb) < 6,
           `translucent background over the blurred backdrop, got rgb(${rr},${gg},${bb})`);
}

// border-radius clips the filtered backdrop. Put the edge just past the
// glass's right side (black up to x = 300, white beyond) so its rounded
// top-right corner sits right beside white.
document.body.innerHTML =
    '<div style="position:absolute;left:0;top:0;width:300px;height:300px;background:rgb(0,0,0)"></div>' +
    '<div style="position:absolute;left:300px;top:0;width:100px;height:300px;background:rgb(255,255,255)"></div>' +
    '<div style="position:absolute;left:100px;top:50px;width:200px;height:200px;backdrop-filter:blur(8px);border-radius:60px"></div>';
for (let i = 0; i < 2; ++i) { advanceTime(16); flush(); }
img = getPixels(0, 0, 400, 300);
// (298, 52) is inside the box but outside its rounded corner: unfiltered
// black, though white is 2px away. (298, 150) is on the straight side,
// inside the glass: the white beyond the box blurs in.
assert(px(img, 298, 52) < 8, `outside the rounded corner the backdrop is untouched, got ${px(img, 298, 52)}`);
assert(px(img, 298, 150) > 40, `on the straight side the blur reaches past the box's edge, got ${px(img, 298, 150)}`);

// opacity mixes the filtered backdrop over the plain one.
img = show('backdrop-filter: blur(8px); opacity: 0.5');
{
    const v = px(img, 196, 150);
    assert(v > 15 && v < 110, `opacity 0.5 halves the blur's effect, got ${v}`);
}

console.log('backdrop-filter OK');
