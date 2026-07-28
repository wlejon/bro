// A clip that was recorded sideways plays the right way up.
//
// Phones do not rotate the pixels; they record landscape frames and write the
// correction into the container. bro's media API had nowhere to put that, so
// no backend could report it and a portrait clip played on its side, laid out
// as though it were landscape. TrackInfo::rotationDegrees is the field, and
// <video> presents it: the natural size swaps at a quarter turn, and the
// picture is turned by a transform on the quad it is drawn as, never by a pass
// over the pixels.
//
// Written against the WebM writer and reader because that is the pair bro
// owns: the rotation goes into the Video Projection element's pose roll on the
// way out and comes back off it on the way in. Existing files are unaffected —
// a rotation of 0 writes no Projection element at all.

const os = require('os');
const path = require('path');
const fs = require('fs');

const W = 64, H = 32, FPS = 10, N = 6;
const made = [];

// Frames are LANDSCAPE (64x32) whatever the rotation says. That is the whole
// point: what is in the file is what the sensor produced.
function encode(rotation) {
    const p = path.join(os.tmpdir(),
        'bro_rot' + rotation + '_' + Date.now() + '.webm');
    const cfg = { path: p, width: W, height: H, fps: FPS, quality: 'realtime' };
    if (rotation !== 0) cfg.rotation = rotation;
    const enc = new VideoEncoder(cfg);
    const px = new Uint8Array(W * H * 4);
    // Left half red, right half blue — asymmetric, so a turn the wrong way
    // does not look like a turn the right way.
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

function open(rotation, left) {
    const v = document.createElement('video');
    document.body.appendChild(v);
    flush();
    v.src = encode(rotation);
    assert(v.readyState >= 1, 'metadata ready for the ' + rotation + ' clip');
    v.style.cssText = 'position:absolute;top:20px;left:' + left + 'px;width:' +
                      v.videoWidth + 'px;height:' + v.videoHeight + 'px;';
    return v;
}

// ── the natural size is the size it is SHOWN at ───────────────────────────

const upright = open(0, 20);
assert(upright.videoRotation === 0, 'an untagged clip reports no rotation');
assert(upright.videoWidth === W && upright.videoHeight === H,
       'and its natural size is the frame size (' +
       upright.videoWidth + 'x' + upright.videoHeight + ')');

const quarter = open(90, 140);
assert(quarter.videoRotation === 90, 'a quarter-turned clip reports 90');
assert(quarter.videoWidth === H && quarter.videoHeight === W,
       'and its natural size is swapped (' + quarter.videoWidth + 'x' +
       quarter.videoHeight + ', want ' + H + 'x' + W + ')');

const half = open(180, 260);
assert(half.videoRotation === 180, 'a half-turned clip reports 180');
assert(half.videoWidth === W && half.videoHeight === H,
       'and a half turn does NOT swap the size (' +
       half.videoWidth + 'x' + half.videoHeight + ')');

const threeQuarter = open(270, 380);
assert(threeQuarter.videoRotation === 270, 'a three-quarter turn reports 270');
assert(threeQuarter.videoWidth === H && threeQuarter.videoHeight === W,
       'and swaps the size too');

flush();

// ── layout follows, because the intrinsic size is what it lays out against ──

const qbox = quarter.getBoundingClientRect();
assert(qbox.height > qbox.width,
       'a portrait clip lays out portrait (' + qbox.width.toFixed(0) + 'x' +
       qbox.height.toFixed(0) + ')');
const ubox = upright.getBoundingClientRect();
assert(ubox.width > ubox.height, 'and a landscape one lays out landscape');

// ── the picture is actually turned ────────────────────────────────────────
// Left half red / right half blue in the file. Turned 90 degrees clockwise,
// the red half is on TOP. One screenshot, then read the drawn pixels back —
// the same shot-decode-scan pattern tests/headless/test_border_image.js uses.

flush();
const SHOT = path.join(os.tmpdir(), 'bro_rotation_shot_' + Date.now() + '.png');
screenshot(SHOT);
made.push(SHOT);
const img = new Image();
img.src = SHOT;
assert(img.naturalWidth > 0, 'screenshot decodes');
const cnv = document.createElement('canvas');
cnv.width = img.naturalWidth;
cnv.height = img.naturalHeight;
const cx = cnv.getContext('2d');
cx.drawImage(img, 0, 0);
const shot = cx.getImageData(0, 0, cnv.width, cnv.height);

function px(x, y) {
    const o = (Math.round(y) * cnv.width + Math.round(x)) * 4;
    return { r: shot.data[o], g: shot.data[o + 1], b: shot.data[o + 2] };
}
// Sample well inside each half so codec ringing at the seam cannot decide it.
function quadrants(v) {
    const r = v.getBoundingClientRect();
    const at = (fx, fy) => px(r.left + r.width * fx, r.top + r.height * fy);
    return { top: at(0.5, 0.2), bottom: at(0.5, 0.8),
             left: at(0.2, 0.5), right: at(0.8, 0.5) };
}
const redder = (c) => c.r > c.b + 40;
const bluer  = (c) => c.b > c.r + 40;
const show = (c) => '(' + c.r + ',' + c.g + ',' + c.b + ')';

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

s = quadrants(threeQuarter);
assert(bluer(s.top) && redder(s.bottom),
       'turned 270 clockwise: the red half is at the bottom — got ' +
       show(s.top) + ' and ' + show(s.bottom));

// ── playback is untouched by any of it ────────────────────────────────────

assert(quarter.duration > 0, 'a rotated clip still reports a duration');
quarter.currentTime = 3 / FPS;
assert(Math.abs(quarter.currentTime - 3 / FPS) < 0.02,
       'and still seeks (' + quarter.currentTime.toFixed(3) + 's)');
assert(quarter.videoWidth === H,
       'and does not change size when the frame does');

for (const v of [upright, quarter, half, threeQuarter]) v.remove();
for (const f of made) { try { fs.unlinkSync(f); } catch (e) {} }
console.log('PASS: <video> rotation');
