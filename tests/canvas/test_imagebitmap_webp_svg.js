// createImageBitmap(Blob) and drawImage(<img>) for WebP and SVG sources.
//
// The image decode ladder behind every host image (Image / <img> /
// createImageBitmap) is one function: WebP (bro's own libwebp), then the
// broimage codecs (PNG/JPEG/...), then SVG rasterized by the svg module. The
// pre-bronze bindings decoded all three for a Blob and for an <img src="x.svg">
// handed to drawImage; the port had dropped WebP and SVG from both.

const fs = require('fs');
const os = require('os');
const path = require('path');

// 1x1 lossless WebP, white at partial alpha (from tests/dom/test_image_webp.js).
const WEBP_B64 = 'UklGRh4AAABXRUJQVlA4TBEAAAAvAAAAEAfQ//73v3+BiOh/AAA=';
const WEBP = new Uint8Array(Buffer.from(WEBP_B64, 'base64'));

const SVG_TEXT =
    '<svg xmlns="http://www.w3.org/2000/svg" width="24" height="16">' +
    '<rect x="0" y="0" width="24" height="16" fill="#00ff00"/></svg>';

function pixelAt(bmpOrImg, w, h, x, y) {
    const c = document.createElement('canvas');
    c.width = w; c.height = h;
    const ctx = c.getContext('2d');
    ctx.drawImage(bmpOrImg, 0, 0);
    const d = ctx.getImageData(x, y, 1, 1).data;
    return { r: d[0], g: d[1], b: d[2], a: d[3] };
}

await (async function () {
    // --- WebP blob -> ImageBitmap ---
    const webpBmp = await createImageBitmap(new Blob([WEBP], { type: 'image/webp' }));
    assert(webpBmp instanceof ImageBitmap, 'WebP blob decodes to an ImageBitmap');
    assert(webpBmp.width === 1 && webpBmp.height === 1, 'WebP dimensions 1x1, got ' + webpBmp.width + 'x' + webpBmp.height);

    // --- SVG blob -> ImageBitmap (rasterized at its intrinsic size) ---
    const svgBmp = await createImageBitmap(new Blob([SVG_TEXT], { type: 'image/svg+xml' }));
    assert(svgBmp instanceof ImageBitmap, 'SVG blob decodes to an ImageBitmap');
    assert(svgBmp.width === 24 && svgBmp.height === 16, 'SVG intrinsic size 24x16, got ' + svgBmp.width + 'x' + svgBmp.height);
    const p = pixelAt(svgBmp, 24, 16, 12, 8);
    assert(p.g > 200 && p.r < 40 && p.b < 40 && p.a > 200, 'SVG rasterized green, got ' + JSON.stringify(p));

    // --- <img src="*.svg"> as a drawImage / createImageBitmap source ---
    const svgPath = path.join(os.tmpdir(), 'bro_ib_svg_' + Date.now() + '.svg');
    fs.writeFileSync(svgPath, SVG_TEXT);
    try {
        const img = new Image();
        img.src = svgPath;
        assert(img.complete === true, 'SVG <img> settled');
        assert(img.width === 24 && img.height === 16, 'SVG <img> decoded its size, got ' + img.width + 'x' + img.height);
        const q = pixelAt(img, 24, 16, 3, 3);
        assert(q.g > 200 && q.r < 40, 'drawImage(<img src=*.svg>) painted the rect, got ' + JSON.stringify(q));
        const fromImg = await createImageBitmap(img);
        assert(fromImg.width === 24 && fromImg.height === 16, 'createImageBitmap(<img svg>) keeps the size');
    } finally {
        try { fs.unlinkSync(svgPath); } catch (e) {}
    }

    // --- <img src="*.webp"> decodes through the same ladder ---
    const webpPath = path.join(os.tmpdir(), 'bro_ib_webp_' + Date.now() + '.webp');
    fs.writeFileSync(webpPath, Buffer.from(WEBP_B64, 'base64'));
    try {
        const img = new Image();
        img.src = webpPath;
        assert(img.complete === true && img.width === 1 && img.height === 1, 'WebP <img> decoded 1x1');
        const fromImg = await createImageBitmap(img);
        assert(fromImg.width === 1, 'createImageBitmap(<img webp>) works');
    } finally {
        try { fs.unlinkSync(webpPath); } catch (e) {}
    }
})();

console.log('imagebitmap webp/svg OK');
