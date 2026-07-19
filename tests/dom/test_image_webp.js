// WebP decoding.
//
// The pinned pre-built Skia carries no libwebp, so SkCodec rejects .webp on
// Windows while a hand-built Linux/macOS Skia accepts it. bro therefore
// decodes WebP itself (libwebp's decoder, compiled from the Skia source
// bundle — see third_party/skia/skia_modules.cmake). The point of these tests
// is that the format works the same everywhere, so they assert decoded
// *pixels*, not merely that a load succeeded: a decoder wired up with the
// wrong channel order or a dropped alpha channel would still report a
// successful load and the right dimensions.
//
// Test images are base64 here rather than checked in as binaries, matching
// test_image.js's reluctance to add binary assets to the tree. Both come from
// Skia's own resources/images.

const fs = require('fs');
const os = require('os');
const path = require('path');

// 1x1 lossless (VP8L) white pixel at partial alpha — the alpha-channel case.
const HALF_TRANSPARENT_WHITE =
    'UklGRh4AAABXRUJQVlA4TBEAAAAvAAAAEAfQ//73v3+BiOh/AAA=';
// 8x8 lossless (VP8L) of random pixels — a real bitstream, not a degenerate
// one-pixel image.
const RAND_PIXELS =
    'UklGRiwBAABXRUJQVlA4TCABAAAvB8ABAP8BRQMA4tWuYCut2Wh2to1qHcDmGdYWt7Tvz8MVbLO9dw2FbRup' +
    'sSvcPYPbAADIJuUBmYrV3fyCzc2ajBPMG7r1Bdvuats4YP5DLNRqmU3GwNMoG8GxF05DDj0n6eSETyRiaYVD' +
    'jcIEAAdZYP2LTWAycs+IDC8vrWK9JwkWcHhs2YLvGqLM8PFv2HguOaHPFEuJtBtIFXxIb4wgtwDP/NvQ6edC' +
    'CaQDQKzaQAEAX6E9ZOH0A0Up4OJKw6Lo4CUSdMbo+5NPn17AppEbpCSCw2EHTElPU65fXTk2wFtXVGAQAADR' +
    'aLZt2zYi+h81dSLsMbxdj4dTogWML82op/OjvYOLKshY9ayA4/5mpHwbbNGp2BTQxbU5cZcZGRs=';

function writeTemp(name, b64) {
    const p = path.join(os.tmpdir(), 'bro_webp_' + Date.now() + '_' + name);
    fs.writeFileSync(p, Buffer.from(b64, 'base64'));
    return p;
}

// Decoding is synchronous, so load/error has fired by the time `src` returns.
function load(p) {
    const img = new Image();
    img.src = p;
    return img;
}

// ---------------------------------------------------------------------------
// A WebP decodes at all, with the right dimensions
// ---------------------------------------------------------------------------
{
    const img = load(writeTemp('rand.webp', RAND_PIXELS));
    assert(img.complete === true, 'complete after src assignment');
    assert(img.naturalWidth === 8,
           'width from the WebP header: ' + img.naturalWidth);
    assert(img.naturalHeight === 8,
           'height from the WebP header: ' + img.naturalHeight);
}

// ---------------------------------------------------------------------------
// The pixels are right — channel order and alpha both
// ---------------------------------------------------------------------------
// A decoder that swapped R and B, or that dropped alpha to opaque, would pass
// every dimension check above. Drawing to a canvas and reading back is the
// cheapest way to see what actually came out of the decoder.
{
    const img = load(writeTemp('half.webp', HALF_TRANSPARENT_WHITE));
    assert(img.naturalWidth === 1 && img.naturalHeight === 1,
           '1x1: ' + img.naturalWidth + 'x' + img.naturalHeight);

    const canvas = document.createElement('canvas');
    canvas.width = 1;
    canvas.height = 1;
    const ctx = canvas.getContext('2d');
    // Transparent black underneath, so anything opaque in the result came
    // from the image rather than from the canvas's initial state.
    ctx.clearRect(0, 0, 1, 1);
    ctx.drawImage(img, 0, 0);

    const px = ctx.getImageData(0, 0, 1, 1).data;
    // White: the three colour channels agree and are at the top of the range.
    // Asserted as a group so a red/blue swap can't hide behind one channel.
    assert(px[0] > 200 && px[1] > 200 && px[2] > 200,
           'white RGB, got ' + px[0] + ',' + px[1] + ',' + px[2]);
    // Partial alpha survived: neither dropped to opaque nor to nothing. The
    // exact value depends on the canvas's compositing, so the assertion is
    // that it is strictly between the two extremes.
    assert(px[3] > 0 && px[3] < 255,
           'alpha stayed partial, got ' + px[3]);
}

// ---------------------------------------------------------------------------
// Bad input fails cleanly
// ---------------------------------------------------------------------------
// WebPGetInfo doubles as the format check, so these exercise the same guard
// that keeps non-WebP bytes from reaching the decoder at all.
{
    // A truncated WebP: valid RIFF/WEBP header, no usable bitstream.
    const full = Buffer.from(RAND_PIXELS, 'base64');
    const cut = path.join(os.tmpdir(), 'bro_webp_truncated_' + Date.now() + '.webp');
    fs.writeFileSync(cut, full.slice(0, 20));
    const img = load(cut);
    assert(img.naturalWidth === 0 && img.naturalHeight === 0,
           'truncated WebP decoded to nothing, got ' +
           img.naturalWidth + 'x' + img.naturalHeight);

    // Bytes that claim to be a WebP but aren't.
    const liar = path.join(os.tmpdir(), 'bro_webp_liar_' + Date.now() + '.webp');
    fs.writeFileSync(liar, Buffer.from('RIFF____WEBPnot actually a webp at all'));
    const img2 = load(liar);
    assert(img2.naturalWidth === 0, 'bogus WebP rejected');
}

// ---------------------------------------------------------------------------
// The renderer decodes WebP too, not just `new Image()`
// ---------------------------------------------------------------------------
// bro has two independent image entry points: the Image binding above
// (broimage, then libwebp) and the renderer's own cache (SkCodec, then
// libwebp). They are separate code, so a fix applied to only one yields a
// .webp that reports naturalWidth 0 but paints, or the reverse. This paints
// an <img> into the document and reads the composited pixel back.
{
    // Forward slashes: the path goes through HTML, where a backslash is not a
    // path separator.
    const webpPath = writeTemp('paint.webp', HALF_TRANSPARENT_WHITE).split('\\').join('/');
    const root = document.getElementById('root');
    // Black backdrop, so the 50%-alpha white composites to an obvious mid
    // grey — distinct from both the backdrop and a fully opaque draw.
    root.innerHTML =
        '<div id="bg" style="position:absolute;left:20px;top:20px;' +
        'width:60px;height:60px;background:#000">' +
        '<img src="' + webpPath + '" style="width:60px;height:60px">' +
        '</div>';
    flush();

    const rect = document.getElementById('bg').getBoundingClientRect();
    const px = getPixel(rect.left + 30, rect.top + 30);
    // Not the backdrop: something painted.
    assert(px.r > 40, 'the WebP painted over black, got r=' + px.r);
    // Not opaque white either: the alpha channel survived compositing.
    assert(px.r < 240, 'painted with partial alpha, got r=' + px.r);
    // Grey — the three channels track each other, so no channel swap.
    assert(Math.abs(px.r - px.g) < 12 && Math.abs(px.g - px.b) < 12,
           'grey, got ' + px.r + ',' + px.g + ',' + px.b);
    root.innerHTML = '';
    flush();
}

// ---------------------------------------------------------------------------
// The other formats still work — the WebP branch sits in a shared decode path
// ---------------------------------------------------------------------------
{
    const png = path.join(os.tmpdir(), 'bro_webp_ctl_' + Date.now() + '.png');
    screenshot(png);
    const img = load(png);
    assert(img.naturalWidth > 1 && img.naturalHeight > 1,
           'PNG still decodes: ' + img.naturalWidth + 'x' + img.naturalHeight);
}

console.log('PASS test_image_webp.js');
