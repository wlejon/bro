// A filmstrip of a clip recorded sideways comes out the right way up.
//
// tests/video/test_rotation.js covers the other half of this: <video> reports
// the swapped natural size and turns the quad it draws. A strip cannot do
// that. It is a baked RGBA image with nothing downstream to rotate, so
// bro.media.thumbnails() has to turn the pixels itself — and it did not, which
// left a row of landscape frames lying on their side underneath a portrait
// picture in every timeline that drew one.
//
// Written against the WebM writer and reader because that is the pair bro
// owns: the rotation goes out in the Video Projection element's pose roll and
// comes back off it as TrackInfo::rotationDegrees.

const os = require('os');
const path = require('path');
const fs = require('fs');

const W = 64, H = 32, FPS = 10, N = 8;
const made = [];

// Frames are LANDSCAPE (64x32) whatever the rotation says — what is in the
// file is what the sensor produced. Left half red, right half blue, so a turn
// the wrong way cannot look like a turn the right way.
function encode(rotation) {
    const p = path.join(os.tmpdir(),
        'bro_thumbrot' + rotation + '_' + Date.now() + '.webm');
    const cfg = { path: p, width: W, height: H, fps: FPS, quality: 'realtime' };
    if (rotation !== 0) cfg.rotation = rotation;
    const enc = new VideoEncoder(cfg);
    const px = new Uint8Array(W * H * 4);
    for (let yy = 0; yy < H; ++yy) {
        for (let xx = 0; xx < W; ++xx) {
            const i = (yy * W + xx) * 4;
            px[i]     = xx < W / 2 ? 255 : 0;
            px[i + 1] = 0;
            px[i + 2] = xx < W / 2 ? 0 : 255;
            px[i + 3] = 255;
        }
    }
    for (let f = 0; f < N; ++f) enc.addFrameRGBA(px);
    enc.finish();
    assert(fs.existsSync(p), 'encoded a clip tagged ' + rotation + ' degrees');
    made.push(p);
    return p;
}

const COUNT = 4, TH = 32;

function strip(rotation) {
    const s = bro.media.thumbnails(encode(rotation), { count: COUNT, height: TH });
    assert(s, 'thumbnails() returned a strip for the ' + rotation + ' clip');
    assert(s.rotation === rotation,
           'the strip reports the turn it applied (' + s.rotation +
           ', want ' + rotation + ')');
    assert(s.data.length === s.width * s.count * s.height * 4,
           'one image, count thumbnails wide');
    return s;
}

// Sample the first thumbnail as fractions of its own tile, well inside each
// half so codec ringing at the seam cannot decide it.
function quadrants(s) {
    const stripW = s.width * s.count;
    const at = (fx, fy) => {
        const x = Math.min(s.width - 1, Math.round(s.width * fx));
        const y = Math.min(s.height - 1, Math.round(s.height * fy));
        const o = (y * stripW + x) * 4;
        return { r: s.data[o], g: s.data[o + 1], b: s.data[o + 2] };
    };
    return { top: at(0.5, 0.2), bottom: at(0.5, 0.8),
             left: at(0.2, 0.5), right: at(0.8, 0.5) };
}
const redder = (c) => c.r > c.b + 40;
const bluer  = (c) => c.b > c.r + 40;
const show = (c) => '(' + c.r + ',' + c.g + ',' + c.b + ')';

// ── the strip is cut to the DISPLAYED aspect ──────────────────────────────

const upright = strip(0);
assert(upright.width === Math.round(TH * W / H),
       'untagged: the tile follows the frame aspect (' + upright.width +
       'x' + upright.height + ')');
assert(upright.width > upright.height, 'and is landscape, like the frames');

const quarter = strip(90);
assert(quarter.width === Math.round(TH * H / W),
       'a quarter turn cuts the tile to the SWAPPED aspect (' + quarter.width +
       'x' + quarter.height + ', want ' + Math.round(TH * H / W) + 'x' + TH + ')');
assert(quarter.height > quarter.width,
       'so a portrait clip gets portrait thumbnails');

const half = strip(180);
assert(half.width === upright.width && half.height === upright.height,
       'a half turn does NOT swap the tile (' + half.width + 'x' + half.height + ')');

const threeQuarter = strip(270);
assert(threeQuarter.width === quarter.width &&
       threeQuarter.height === quarter.height,
       'a three-quarter turn swaps it like a quarter does');

// ── and the pixels are actually turned ────────────────────────────────────
// Left half red / right half blue in the file. This is the half that a strip
// sized correctly but never rotated would still fail.

let s = quadrants(upright);
assert(redder(s.left) && bluer(s.right),
       'untagged: red on the left, blue on the right — got ' +
       show(s.left) + ' and ' + show(s.right));

s = quadrants(quarter);
assert(redder(s.top) && bluer(s.bottom),
       'turned 90 clockwise: the red half is on top — got ' +
       show(s.top) + ' and ' + show(s.bottom));

s = quadrants(half);
assert(bluer(s.left) && redder(s.right),
       'turned 180: the halves swap sides — got ' +
       show(s.left) + ' and ' + show(s.right));

// The one that catches 90 and 270 being wired the same way round.
s = quadrants(threeQuarter);
assert(bluer(s.top) && redder(s.bottom),
       'turned 270: the red half is on the BOTTOM — got ' +
       show(s.top) + ' and ' + show(s.bottom));

for (const p of made) { try { fs.unlinkSync(p); } catch (e) {} }
console.log('PASS');
