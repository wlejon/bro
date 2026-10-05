// Skia's layers through the frame composite: the page, a 2D canvas, an iframe
// sub-document, at 1x and 2x. With a Vulkan device these are GPU surfaces the
// presenter samples in place (no readback); with BRO_SKIA_GPU=0 they are CPU
// pixels through the same composite. (--no-gpu is the separate direct-raster
// path, which draws neither iframes nor at 2x.) The pixels must agree either
// way, so every probe is a
// flat interior colour (exact up to rounding) or an edge checked for being a
// blend of its two sides, never a particular anti-aliasing.
//
// What a broken GPU path shows here: a layer missing (the clear colour), a
// stale frame (the canvas's first drawing after it was redrawn), an iframe
// that never publishes, or a layer at the wrong scale.

document.body.style.margin = '0';
document.body.style.background = '#ffffff';

function close(px, r, g, b, tol, what) {
    const ok = Math.abs(px.r - r) <= tol && Math.abs(px.g - g) <= tol && Math.abs(px.b - b) <= tol;
    assert(ok, `${what}: expected ~(${r},${g},${b}), got (${px.r},${px.g},${px.b},${px.a})`);
}

// ── The page: solid boxes, a translucent overlay, a rounded corner ─────────
const solid = document.createElement('div');
solid.style.cssText = 'position:absolute;left:10px;top:10px;width:100px;height:60px;background:#c03020';
document.body.appendChild(solid);
const overlay = document.createElement('div');
overlay.style.cssText = 'position:absolute;left:60px;top:40px;width:100px;height:60px;' +
                        'background:rgba(0,0,255,0.5)';
document.body.appendChild(overlay);
const round = document.createElement('div');
round.style.cssText = 'position:absolute;left:200px;top:10px;width:80px;height:80px;' +
                      'background:#108040;border-radius:40px';
document.body.appendChild(round);

// ── A 2D canvas ─────────────────────────────────────────────────────────────
const canvas = document.createElement('canvas');
canvas.width = 120;
canvas.height = 80;
canvas.style.cssText = 'position:absolute;left:10px;top:120px;width:120px;height:80px';
document.body.appendChild(canvas);
const g = canvas.getContext('2d');
g.fillStyle = '#00c000';
g.fillRect(0, 0, 60, 80);
g.fillStyle = '#f0a000';
g.fillRect(60, 0, 60, 80);

// ── An iframe ───────────────────────────────────────────────────────────────
const frame = document.createElement('iframe');
frame.setAttribute('src', 'iframe_child');
frame.style.cssText = 'position:absolute;left:200px;top:120px;width:160px;height:120px;border:0';
document.body.appendChild(frame);
flush();
advanceTime(100);
flush();

function checkPage(scale) {
    const at = ` at ${scale}x`;
    close(getPixel(20, 20), 0xc0, 0x30, 0x20, 1, 'solid box' + at);
    // Half blue over the red box, and over the white page.
    close(getPixel(80, 55), 0x60, 0x18, 0x90, 2, 'overlay over the box' + at);
    close(getPixel(140, 90), 0x80, 0x80, 0xff, 2, 'overlay over the page' + at);
    close(getPixel(240, 50), 0x10, 0x80, 0x40, 1, 'rounded box centre' + at);
    close(getPixel(202, 12), 0xff, 0xff, 0xff, 1, 'outside the rounded corner' + at);
    // On the curve: some blend of the two sides, whatever the AA.
    const edge = getPixel(240, 10);
    assert(edge.g >= 0x80 && edge.r >= 0x10 && edge.r <= 0xff,
           `rounded edge is a blend of green and white${at}: got (${edge.r},${edge.g},${edge.b})`);
    close(getPixel(400, 300), 0xff, 0xff, 0xff, 0, 'bare page' + at);

    close(getPixel(30, 160), 0x00, 0xc0, 0x00, 1, 'canvas left half' + at);
    close(getPixel(110, 160), 0xf0, 0xa0, 0x00, 1, 'canvas right half' + at);

    // The child's body; its box sits under the label at its left edge.
    close(getPixel(340, 220), 0x22, 0x44, 0xaa, 1, 'iframe body' + at);
}

checkPage(1);

// A redraw shows on the next frame, not the image from before it.
g.fillStyle = '#8000c0';
g.fillRect(0, 0, 120, 80);
flush();
close(getPixel(30, 160), 0x80, 0x00, 0xc0, 1, 'redrawn canvas, left');
close(getPixel(110, 160), 0x80, 0x00, 0xc0, 1, 'redrawn canvas, right');
g.fillStyle = '#00c000';
g.fillRect(0, 0, 60, 80);
g.fillStyle = '#f0a000';
g.fillRect(60, 0, 60, 80);
flush();

// The page changes underneath: the layer is redrawn, not a stale copy.
solid.style.background = '#2050d0';
flush();
close(getPixel(20, 20), 0x20, 0x50, 0xd0, 1, 'restyled box');
solid.style.background = '#c03020';
flush();

// A pixel read from the canvas sees what it shows.
const id = g.getImageData(30, 40, 1, 1);
assert(id.data[0] === 0 && id.data[1] === 0xc0 && id.data[2] === 0 && id.data[3] === 255,
       `getImageData: got ${Array.from(id.data)}`);

// At 2x every surface doubles; probes stay in CSS px and see the same page.
setDeviceScaleFactor(2);
flush();
advanceTime(50);
flush();
checkPage(2);
setDeviceScaleFactor(1);
flush();
checkPage(1);
