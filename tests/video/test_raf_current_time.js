// A requestAnimationFrame callback and the line after advanceTime() read the
// same <video>.currentTime.
//
// The windowed frame pumps media once, before rAF, and everything the frame
// runs afterwards sees that instant. Headless used to pump it twice per step:
// once before the callbacks and again in the step's closing flush(), and a
// headless pump ADVANCES the video. So the picture moved after rAF had read it
// — catching up with an audio-slaved clock, or with a wall clock that ran on
// while the callbacks did — and a script that read currentTime in rAF and again
// after advanceTime got two answers a picture apart, whenever the step's tail
// crossed a picture boundary. Measured on a clip with Opus audio: 23 of 60
// steps disagreed.
//
// Covered: a clip with sound (clock slaved to the audio advanceTime renders), a
// clip with none (host wall clock), paused, seeking and ended elements, and a
// flush() outside advanceTime still advancing a playing video.

const os = require('os');
const path = require('path');
const fs = require('fs');

const W = 64, H = 64, FPS = 30, RATE = 48000;

function encode(name, seconds, withSound) {
    const src = path.join(os.tmpdir(), 'bro_raf_ct_' + name + '_' + Date.now() + '.webm')
                    .split('\\').join('/');
    const opts = { path: src, width: W, height: H, fps: FPS, fpsDen: 1, quality: 'realtime' };
    if (withSound) Object.assign(opts, { audioSampleRate: RATE, audioChannels: 1, audioBitrateKbps: 64 });
    const enc = new VideoEncoder(opts);
    const px = new Uint8Array(W * H * 4);
    const frames = Math.round(FPS * seconds);
    for (let f = 0; f < frames; ++f) {
        for (let i = 0; i < W * H; ++i) {
            px[i * 4] = (f * 8) & 255; px[i * 4 + 1] = 90; px[i * 4 + 2] = 180; px[i * 4 + 3] = 255;
        }
        enc.addFrameRGBA(px);
    }
    if (withSound) {
        const n = Math.round(RATE * seconds);
        const pcm = new Float32Array(n);
        for (let i = 0; i < n; ++i) pcm[i] = 0.25 * Math.sin(2 * Math.PI * 440 * i / RATE);
        enc.addAudioFramesPCM(pcm, n);
    }
    enc.finish();
    assert(fs.existsSync(src), 'encoded ' + name);
    return src;
}

const withSound = encode('sound', 3, true);
const silent = encode('silent', 3, false);

function open(src) {
    const v = document.createElement('video');
    document.body.appendChild(v);
    flush();
    v.src = src;
    for (let i = 0; i < 40 && v.readyState < 2; i++) { advanceTime(16); flush(); }
    assert(v.readyState >= 2, 'opened ' + path.basename(src));
    return v;
}

// Step `steps` times by odd amounts (7..19 ms, so some steps split in two and
// the step tails fall all over the picture grid), comparing the last rAF read
// with the read after advanceTime. `work` runs inside each callback, after the
// read: a wall-clock video moves on while it does.
function compare(v, label, steps, work) {
    let inRaf = -1, alive = true, mismatches = 0, first = '';
    const tick = () => {
        if (!alive) return;
        inRaf = v.currentTime;
        if (work) work();
        requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);
    const start = v.currentTime;
    for (let i = 0; i < steps; i++) {
        advanceTime(7 + (i % 5) * 3);
        const after = v.currentTime;
        if (Math.abs(after - inRaf) > 1e-9) {
            if (!mismatches) first = ' (first: rAF ' + inRaf.toFixed(4) + ' vs after ' + after.toFixed(4) + ')';
            mismatches++;
        }
    }
    alive = false;
    advanceTime(16);
    assert(mismatches === 0, label + ': rAF and after-advanceTime reads agree, ' +
           mismatches + '/' + steps + ' differ' + first);
    return v.currentTime - start;
}

// ── a clip with sound: the clock follows the audio advanceTime renders ─────
{
    const v = open(withSound);
    v.play();
    const moved = compare(v, 'with sound', 60);
    assert(moved > 0.1, 'with sound: playback advanced (' + moved.toFixed(3) + 's)');
    v.pause();
    v.remove();
}

// ── a clip with no sound: the clock is the host's, and runs during rAF ─────
{
    const v = open(silent);
    v.play();
    const moved = compare(v, 'silent', 60, () => wallSleep(4));
    assert(moved > 0.1, 'silent: playback advanced (' + moved.toFixed(3) + 's)');

    // ── paused: nothing moves, and both reads agree ────────────────────────
    v.pause();
    const held = v.currentTime;
    const movedPaused = compare(v, 'paused', 20, () => wallSleep(4));
    assert(movedPaused === 0, 'paused: currentTime stays at ' + held.toFixed(3) +
           ' (moved ' + movedPaused.toFixed(3) + ')');

    // ── seeking: the target is readable on the next line and in the frame ──
    v.currentTime = 1.5;
    assert(Math.abs(v.currentTime - 1.5) < 1 / FPS + 1e-6,
           'seek lands on the picture 1.5s falls in (' + v.currentTime.toFixed(3) + ')');
    compare(v, 'seeked, paused', 10, () => wallSleep(4));
    v.play();
    v.currentTime = 0.5;
    compare(v, 'seeking while playing', 30, () => wallSleep(4));

    // ── flush() outside advanceTime still advances a playing video ─────────
    const before = v.currentTime;
    wallSleep(200);
    flush();
    assert(v.currentTime > before + 0.1,
           'a script-driven flush() advances playback (' + before.toFixed(3) +
           ' -> ' + v.currentTime.toFixed(3) + ')');

    // ── ended: parked at the end, and both reads agree ─────────────────────
    v.currentTime = v.duration - 0.1;
    const t0 = Date.now();
    while (!v.ended && Date.now() - t0 < 5000) { wallSleep(20); advanceTime(16); }
    assert(v.ended, 'the silent clip ends');
    const end = v.currentTime;
    compare(v, 'ended', 20, () => wallSleep(4));
    assert(v.currentTime === end, 'ended: currentTime stays at the end (' +
           end.toFixed(3) + ' -> ' + v.currentTime.toFixed(3) + ')');
    v.remove();
}

for (const f of [withSound, silent]) {
    try { fs.unlinkSync(f.split('/').join(path.sep)); } catch (e) {}
}

console.log('PASS: rAF and advanceTime read the same video currentTime');
