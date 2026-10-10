// <img> plays an animated GIF (render/animated_image.h): frame 0 first and as
// the natural size, then each frame for its stored delay on the engine's
// clock (advanceTime here), a delay of 10 ms or less shown for 100 ms, the
// loop count honoured. A CSS background of the same source shares the
// timeline; canvas drawImage draws the current frame. An animation that is
// not painted on screen (off screen, display:none, detached) asks for no
// more frames, and in the windowed frame loop (runFrames) it neither paints
// nor keeps frames coming. A big animation is streamed with a bounded
// decode-ahead window and plays the same.

const fs = require('fs');
const path = require('path');
const os = require('os');

// ── GIF writer: literal LZW codes, a clear code before the table would grow ──
function gif(W, H, gct, loop, frames) {
    const o = [0x47, 0x49, 0x46, 0x38, 0x39, 0x61];
    const u16 = (v) => o.push(v & 255, (v >> 8) & 255);
    const bitsFor = (n) => { let b = 1; while ((1 << b) < n) b++; return b; };
    const table = (t) => { const b = bitsFor(t.length / 3); o.push(...t); for (let i = t.length; i < 3 << b; i++) o.push(0); };
    u16(W); u16(H);
    o.push(0xF0 | (bitsFor(gct.length / 3) - 1), 0, 0);
    table(gct);
    if (loop >= 0) { o.push(0x21, 0xFF, 11, ...[...'NETSCAPE2.0'].map((c) => c.charCodeAt(0)), 3, 1); u16(loop); o.push(0); }
    for (const f of frames) {
        o.push(0x21, 0xF9, 4, (f.disposal || 0) << 2);
        u16(f.delay || 0);
        o.push(0, 0, 0x2C);
        u16(0); u16(0); u16(W); u16(H);
        o.push(0);
        const min = Math.max(2, bitsFor(gct.length / 3)), clear = 1 << min, size = min + 1;
        const bytes = [];
        let acc = 0, n = 0;
        const code = (c) => { acc |= c << n; n += size; while (n >= 8) { bytes.push(acc & 255); acc >>>= 8; n -= 8; } };
        code(clear);
        let since = 0;
        for (let i = 0; i < W * H; i++) { if (since === clear - 2) { code(clear); since = 0; } code(f.color); since++; }
        code(clear + 1);
        if (n) bytes.push(acc & 255);
        o.push(min);
        for (let i = 0; i < bytes.length; i += 255) { const s = bytes.slice(i, i + 255); o.push(s.length, ...s); }
        o.push(0);
    }
    o.push(0x3B);
    return new Uint8Array(o);
}
const RGB = [[255, 0, 0], [0, 255, 0], [0, 0, 255], [255, 255, 0], [255, 0, 255], [0, 255, 255]];
const gct = RGB.flat().concat([255, 255, 255, 0, 0, 0]);
const NAMES = ['red', 'green', 'blue', 'yellow', 'magenta', 'cyan'];
const colorAt = (x, y) => {
    const p = getPixel(x, y);
    const i = RGB.findIndex(([r, g, b]) => Math.abs(p.r - r) < 40 && Math.abs(p.g - g) < 40 && Math.abs(p.b - b) < 40);
    return i < 0 ? JSON.stringify(p) : NAMES[i];
};

const dir = path.join(os.tmpdir(), 'bro_img_anim_' + process.pid + '_' + Date.now());
fs.mkdirSync(dir, { recursive: true });
const write = (name, bytes) => { const f = path.join(dir, name).replace(/\\/g, '/'); fs.writeFileSync(f, bytes); return f; };

// a.gif: red 100 ms, green 10 ms (shown for 100), blue 200 ms; stored loop 1: plays twice.
const aFile = write('a.gif', gif(20, 20, gct, 1, [{ color: 0, delay: 10 }, { color: 1, delay: 1 }, { color: 2, delay: 20 }]));
// b.gif: red / blue, 100 ms each, forever.
const bFile = write('b.gif', gif(20, 20, gct, 0, [{ color: 0, delay: 10 }, { color: 2, delay: 10 }]));

document.body.style.margin = '0';
document.body.style.background = 'rgb(0,0,0)';
document.body.innerHTML =
    '<img id="a" src="' + aFile + '" style="position:absolute;left:0;top:0">' +
    '<div id="bg" style="position:absolute;left:40px;top:0;width:20px;height:20px;background-image:url(' + aFile + ')"></div>';
flush();
const a = document.getElementById('a');
await a.decode();
flush();
assert(a.complete && a.naturalWidth === 20 && a.naturalHeight === 20, 'the GIF loads at its canvas size');

// Frame 0 first; the timeline starts with the first paint.
assert(colorAt(5, 5) === 'red', 'frame 0 shows first: ' + colorAt(5, 5));
let s = __host.imageAnimations(a);
assert(s.frameCount === 3 && s.cachesAll && s.frame === 0, 'a small animation caches its frames: ' + JSON.stringify(s));
assert(s.scheduled >= 1 && s.nextDueInMs > 0 && s.nextDueInMs <= 100, 'painting it asks for the next frame: ' + JSON.stringify(s));
advanceTime(50);
assert(colorAt(5, 5) === 'red', 'still frame 0 at 50 ms');
advanceTime(100);   // 150: frame 1 runs 100..200 (10 ms clamps to 100)
assert(colorAt(5, 5) === 'green', 'frame 1 at 150 ms (a 10 ms delay shows for 100): ' + colorAt(5, 5));
assert(colorAt(45, 5) === 'green', 'the CSS background shares the timeline: ' + colorAt(45, 5));
advanceTime(100);   // 250: frame 2 runs 200..400
assert(colorAt(5, 5) === 'blue', 'frame 2 at 250 ms: ' + colorAt(5, 5));
advanceTime(200);   // 450: the second play
assert(colorAt(5, 5) === 'red', 'the second play starts over: ' + colorAt(5, 5));
advanceTime(400);   // 850: two plays done (800 ms)
assert(colorAt(5, 5) === 'blue', 'after its loop count it rests on the last frame: ' + colorAt(5, 5));
s = __host.imageAnimations(a);
assert(s.frame === 2 && s.scheduled === 0, 'a finished animation asks for nothing: ' + JSON.stringify(s));
assert(a.naturalWidth === 20 && a.complete, 'natural size and complete are unchanged by playing');

// ── b.gif: on screen, off screen, display:none, detached ────────────────────
document.body.innerHTML = '<img id="b" src="' + bFile + '" style="position:absolute;left:0;top:40px">' +
                          '<canvas id="c" width="4" height="4" style="position:absolute;left:100px;top:0"></canvas>';
flush();
const b = document.getElementById('b');
await b.decode();
flush();
assert(colorAt(5, 45) === 'red', 'b starts on frame 0');
advanceTime(120);
assert(colorAt(5, 45) === 'blue', 'b on frame 1 at 120 ms');
assert(__host.imageAnimations().scheduled === 1, 'b, painted, waits for its next frame');

// Canvas drawImage takes the frame showing now.
const ctx = document.getElementById('c').getContext('2d');
ctx.drawImage(b, 0, 0, 4, 4);
let d = ctx.getImageData(1, 1, 1, 1).data;
assert(d[2] > 200 && d[0] < 50, 'drawImage of the animated <img> draws its current frame (blue): ' + Array.from(d));

const quiet = (what) => {
    // The repaint already asked for fires once; then nothing is asked again.
    advanceTime(300);
    colorAt(5, 45);
    const s1 = __host.imageAnimations();
    advanceTime(1000);
    colorAt(5, 45);
    const s2 = __host.imageAnimations();
    assert(s2.scheduled === 0 && s2.repaints === s1.repaints && s2.paints === s1.paints,
           what + ': no frames are asked for: ' + JSON.stringify([s1, s2]));
};
b.style.top = '3000px';
flush();
quiet('off screen');

b.style.top = '40px';
flush();
colorAt(5, 45);
assert(__host.imageAnimations().scheduled === 1, 'back on screen it asks again');
b.style.display = 'none';
flush();
quiet('display:none');

b.style.display = '';
flush();
colorAt(5, 45);
assert(__host.imageAnimations().scheduled === 1, 'shown again it asks again');
document.body.innerHTML = '<div style="width:10px;height:10px;overflow:hidden"><div style="height:500px"></div>' +
                          '<img id="b2" src="' + bFile + '"></div>';
flush();
colorAt(1, 1);
quiet('clipped out by its overflow:hidden parent');

// ── The windowed frame loop: frames only when one is due, none off screen ──
document.body.innerHTML = '<img id="b3" src="' + bFile + '" style="position:absolute;left:0;top:40px">';
flush();
colorAt(5, 45);
let before = __host.imageAnimations();
const run = runFrames(60, { stepMs: 16 });   // ~960 ms: about 10 frame changes
let after = __host.imageAnimations();
const painted = after.paints - before.paints;
assert(painted >= 5 && painted <= 20, 'the windowed loop repaints when a frame is due, not every frame: ' +
       painted + ' paints in ' + JSON.stringify(run));
document.getElementById('b3').style.top = '3000px';
flush();
runFrames(10, { stepMs: 16 });
before = __host.imageAnimations();
const idle = runFrames(60, { stepMs: 16 });
after = __host.imageAnimations();
assert(after.paints === before.paints && after.scheduled === 0 && after.repaints === before.repaints,
       'off screen the loop paints no frames: ' + JSON.stringify([before, after, idle]));

// ── A big animation streams through a bounded window and plays the same ───
__host.setImageAnimationBudgetMB(0);
const cFile = write('c.gif', gif(30, 30, gct, 0, [0, 1, 2, 3, 4, 5].map((c) => ({ color: c, delay: 10 }))));
document.body.innerHTML = '<img id="cc" src="' + cFile + '" style="position:absolute;left:0;top:0">';
flush();
const cc = document.getElementById('cc');
await cc.decode();
flush();
assert(colorAt(5, 5) === 'red', 'streamed: frame 0');
s = __host.imageAnimations(cc);
assert(!s.cachesAll && s.frameCount === 6, 'a budget of 0 streams it: ' + JSON.stringify(s));
const seen = [];
for (let i = 0; i < 8; i++) {
    advanceTime(100);
    seen.push(colorAt(5, 5));
}
assert(seen.join() === 'green,blue,yellow,magenta,cyan,red,green,blue', 'streamed frames in order, looping: ' + seen.join());
__host.setImageAnimationBudgetMB(32);

fs.rmSync(dir, { recursive: true, force: true });
