// <img> element layout and painting.
//
// An <img> is a REPLACED element, and layout can only treat it as one if it
// can answer "how big is it?". For a raster image the answer is the decoded
// pixel size, which is only knowable by reading the file — so bro probes the
// header when `src` is set and caches it on the element.
//
// Before that probe existed, intrinsicSize() could only answer for width/height
// attributes and SVG data URLs. Everything else reported NO intrinsic size,
// which made the <img> a non-replaced inline box: zero-sized, with CSS
// width/height correctly ignored (they don't apply to non-replaced inlines).
// The visible symptom was that a plain `<img src="photo.png">` simply did not
// appear, and neither did one sized purely in CSS. No test in the suite
// covered DOM <img> rendering, which is why it went unnoticed.
//
// So these tests care about two things: that an <img> takes the right BOX, and
// that pixels actually land in it.

const fs = require('fs');
const os = require('os');
const path = require('path');

// 40x20 solid rgb(220,40,40) PNG.
const RED_40x20 =
    'iVBORw0KGgoAAAANSUhEUgAAACgAAAAUCAIAAABwJOjsAAAAJElEQVR42mO4o6ExIIhh1OJRi0ct' +
    'HrV41OJRi0ctHrV45FgMAGbCqa5Wls8rAAAAAElFTkSuQmCC';
// 8x8 lossless WebP (from Skia's resources) — the intrinsic-size probe has to
// read WebP headers too, since the stb-backed prober cannot.
const WEBP_8x8 =
    'UklGRiwBAABXRUJQVlA4TCABAAAvB8ABAP8BRQMA4tWuYCut2Wh2to1qHcDmGdYWt7Tvz8MVbLO9dw2FbRup' +
    'sSvcPYPbAADIJuUBmYrV3fyCzc2ajBPMG7r1Bdvuats4YP5DLNRqmU3GwNMoG8GxF05DDj0n6eSETyRiaYVD' +
    'jcIEAAdZYP2LTWAycs+IDC8vrWK9JwkWcHhs2YLvGqLM8PFv2HguOaHPFEuJtBtIFXxIb4wgtwDP/NvQ6edC' +
    'CaQDQKzaQAEAX6E9ZOH0A0Up4OJKw6Lo4CUSdMbo+5NPn17AppEbpCSCw2EHTElPU65fXTk2wFtXVGAQAADR' +
    'aLZt2zYi+h81dSLsMbxdj4dTogWML82op/OjvYOLKshY9ayA4/5mpHwbbNGp2BTQxbU5cZcZGRs=';

function writeTemp(name, b64) {
    const p = path.join(os.tmpdir(), 'bro_img_' + Date.now() + '_' + name);
    fs.writeFileSync(p, Buffer.from(b64, 'base64'));
    // Forward slashes: the src goes through HTML, where a backslash is not a
    // path separator.
    return p.replace(/\\/g, '/');
}

const PNG = writeTemp('red.png', RED_40x20);
const WEBP = writeTemp('rand.webp', WEBP_8x8);
const root = document.getElementById('root');

function show(html) {
    root.innerHTML = html;
    flush();
}

function rectOf(id) {
    return document.getElementById(id).getBoundingClientRect();
}

// Is this pixel the image's red, rather than the page's white background?
function isRed(px) {
    return px.r > 150 && px.g < 120 && px.b < 120;
}

// ---------------------------------------------------------------------------
// An <img> with no attributes and no CSS takes its intrinsic size
// ---------------------------------------------------------------------------
// The case that was silently invisible: nothing tells layout how big this is
// except the file itself.
{
    show('<img id="a" src="' + PNG + '" style="position:absolute;left:20px;top:20px">');
    const r = rectOf('a');
    assert(r.width === 40 && r.height === 20,
           'intrinsic 40x20, got ' + r.width + 'x' + r.height);
    assert(isRed(getPixel(r.left + 20, r.top + 10)),
           'painted, got ' + JSON.stringify(getPixel(r.left + 20, r.top + 10)));
}

// ---------------------------------------------------------------------------
// CSS width/height size the box — they apply because it is replaced
// ---------------------------------------------------------------------------
// This is the same bug wearing a different hat: with no intrinsic size the
// element was a non-replaced inline, and CSS width/height are ignored on
// those. Note there is no display:block here — inline is the default, and an
// inline REPLACED element does take a width.
{
    show('<img id="b" src="' + PNG + '" style="position:absolute;left:20px;top:20px;width:60px;height:60px">');
    const r = rectOf('b');
    assert(r.width === 60 && r.height === 60,
           'CSS-sized 60x60, got ' + r.width + 'x' + r.height);
    assert(isRed(getPixel(r.left + 30, r.top + 30)), 'painted at CSS size');
}

// width/height ATTRIBUTES still win, as they always did.
{
    show('<img id="c" src="' + PNG + '" width="80" height="30" style="position:absolute;left:20px;top:20px">');
    const r = rectOf('c');
    assert(r.width === 80 && r.height === 30,
           'attrs 80x30, got ' + r.width + 'x' + r.height);
}

// A single axis derives the other from the intrinsic aspect ratio (40:20 = 2).
{
    show('<img id="d" src="' + PNG + '" width="80" style="position:absolute;left:20px;top:20px">');
    const r = rectOf('d');
    assert(r.width === 80, 'width honoured, got ' + r.width);
    assert(r.height === 40,
           'height from the 2:1 intrinsic ratio, got ' + r.height);
}

// ---------------------------------------------------------------------------
// Other src flavours reach the same probe
// ---------------------------------------------------------------------------
{
    show('<img id="e" src="data:image/png;base64,' + RED_40x20 +
         '" style="position:absolute;left:20px;top:20px">');
    const r = rectOf('e');
    assert(r.width === 40 && r.height === 20,
           'data: URL intrinsic 40x20, got ' + r.width + 'x' + r.height);
    assert(isRed(getPixel(r.left + 20, r.top + 10)), 'data: URL painted');
}
{
    // WebP headers need libwebp; the stb-backed prober returns nothing for them.
    show('<img id="f" src="' + WEBP + '" style="position:absolute;left:20px;top:20px">');
    const r = rectOf('f');
    assert(r.width === 8 && r.height === 8,
           'WebP intrinsic 8x8, got ' + r.width + 'x' + r.height);
}

// ---------------------------------------------------------------------------
// Changing src re-probes
// ---------------------------------------------------------------------------
// The size is cached against the src it was read for, and a plain attribute
// write does not run the pass that refreshes it — so a src change has to mark
// the element for restructuring or the box keeps its predecessor's size.
{
    show('<img id="g" src="' + WEBP + '" style="position:absolute;left:20px;top:20px">');
    assert(rectOf('g').width === 8, 'starts at the WebP size');

    document.getElementById('g').setAttribute('src', PNG);
    flush();
    const r = rectOf('g');
    assert(r.width === 40 && r.height === 20,
           're-probed after src change, got ' + r.width + 'x' + r.height);
    assert(isRed(getPixel(r.left + 20, r.top + 10)), 'new image painted');
}

// ---------------------------------------------------------------------------
// A broken src is zero-sized, not a crash and not a stale size
// ---------------------------------------------------------------------------
{
    show('<img id="h" src="definitely-not-here.png" style="position:absolute;left:20px;top:20px">');
    const r = rectOf('h');
    assert(r.width === 0 && r.height === 0,
           'missing file gives an empty box, got ' + r.width + 'x' + r.height);
}
{
    // Sized, then pointed at nothing: the old dimensions must not survive.
    show('<img id="i" src="' + PNG + '" style="position:absolute;left:20px;top:20px">');
    assert(rectOf('i').width === 40, 'sized first');
    document.getElementById('i').setAttribute('src', 'also-not-here.png');
    flush();
    assert(rectOf('i').width === 0,
           'stale size dropped, got ' + rectOf('i').width);
}

root.innerHTML = '';
flush();

console.log('PASS test_img_element.js');
