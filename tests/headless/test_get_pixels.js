// getPixels(x, y, w, h): a block of getPixel() probes read from one
// composite. Every texel must be exactly what getPixel answers for the same
// document coordinate, including the zeroes outside the document.

document.body.style.cssText = 'margin:0;background:rgb(255,255,255);';
document.body.innerHTML =
    '<div style="position:absolute;left:10px;top:20px;width:30px;height:15px;' +
    'background:rgb(255,0,0)"></div>' +
    '<div style="position:absolute;left:40px;top:20px;width:30px;height:15px;' +
    'background:rgb(0,0,255)"></div>';
flush();

const img = getPixels(5, 15, 70, 25);
assert(img.width === 70 && img.height === 25, 'getPixels reports its size');
assert(img.data instanceof Uint8ClampedArray && img.data.length === 70 * 25 * 4,
       'getPixels data is RGBA bytes, ImageData-shaped');

const texel = (im, x, y) => {
    const i = (y * im.width + x) * 4;
    return [im.data[i], im.data[i + 1], im.data[i + 2], im.data[i + 3]];
};
let mismatches = 0;
for (let y = 0; y < img.height; y += 3) {
    for (let x = 0; x < img.width; x += 3) {
        const p = getPixel(5 + x, 15 + y);
        const t = texel(img, x, y);
        if (t[0] !== p.r || t[1] !== p.g || t[2] !== p.b || t[3] !== p.a) mismatches++;
    }
}
assert(mismatches === 0, 'every getPixels texel equals getPixel there, mismatches: ' + mismatches);

const red = texel(img, 15, 10), blue = texel(img, 50, 10), white = texel(img, 2, 2);
assert(red[0] > 240 && red[2] < 16, 'red box read back: ' + red);
assert(blue[2] > 240 && blue[0] < 16, 'blue box read back: ' + blue);
assert(white[0] > 240 && white[1] > 240 && white[2] > 240, 'background read back: ' + white);

// A block straddling the document's top-left corner: the outside is zeroes,
// exactly as getPixel reports out-of-document coordinates.
const edge = getPixels(-4, -4, 8, 8);
assert(texel(edge, 0, 0).every(v => v === 0), 'out-of-document texels are zero');
const inside = texel(edge, 6, 6), probe = getPixel(2, 2);
assert(inside[0] === probe.r && inside[3] === probe.a, 'in-document texels of an edge block are read');

let threw = false;
try { getPixels(0, 0, 0, 10); } catch (e) { threw = true; }
assert(threw, 'getPixels rejects an empty block');

console.log('getPixels OK');
