// `video.setAttribute('src', ...)` loads, exactly as `video.src = ...` does.
//
// Both spellings are the same content attribute, and a page that builds its
// markup generically — a templating helper that walks {src: url} and calls
// setAttribute for every key — only ever uses the attribute one. The property
// setter creates the media control and calls load(); the attribute setter has
// to do the same for an element that already has a control, which is what the
// old binding did and what the bronze port dropped: the attribute changed, the
// file was never opened, and `loadedmetadata` never fired.

const os = require('os');
const path = require('path');
const fs = require('fs');

const file = path.join(os.tmpdir(), 'bro_drift_setattr_' + Date.now() + '.webm');
const W = 64, H = 64, FRAMES = 5;

const enc = new VideoEncoder({ path: file, width: W, height: H,
                               fps: 5, fpsDen: 1, quality: 'realtime' });
const px = new Uint8Array(W * H * 4);
for (let f = 0; f < FRAMES; f++) {
    for (let i = 0; i < W * H; i++) {
        px[i * 4] = (f * 51) & 255; px[i * 4 + 1] = 90; px[i * 4 + 2] = 180; px[i * 4 + 3] = 255;
    }
    enc.addFrameRGBA(px);
}
enc.finish();
assert(fs.existsSync(file), 'wrote the fixture clip');

const url = file.split('\\').join('/');

function waitForMetadata(v) {
    let ready = false, failed = false;
    v.addEventListener('loadedmetadata', function () { ready = true; });
    v.addEventListener('error', function () { failed = true; });
    const t = Date.now();
    while (!ready && !failed && Date.now() - t < 15000) { sleep(20); flush(); advanceTime(20); }
    return ready;
}

const root = document.getElementById('root');

// --- the property setter: the path that always worked ---------------------
const viaProp = document.createElement('video');
root.appendChild(viaProp);
viaProp.src = url;
assert(waitForMetadata(viaProp), 'the property setter loaded the clip');
assert(viaProp.videoWidth === W, 'and reports the picture size: ' + viaProp.videoWidth);

// --- the attribute setter on an element that already has a control --------
// Assigning `src` once creates the control; the SECOND source arrives through
// setAttribute, which is the drifted path.
const file2 = path.join(os.tmpdir(), 'bro_drift_setattr2_' + Date.now() + '.webm');
const enc2 = new VideoEncoder({ path: file2, width: 32, height: 48,
                                fps: 5, fpsDen: 1, quality: 'realtime' });
const px2 = new Uint8Array(32 * 48 * 4);
for (let f = 0; f < FRAMES; f++) {
    for (let i = 0; i < 32 * 48; i++) {
        px2[i * 4] = 10; px2[i * 4 + 1] = (f * 41) & 255; px2[i * 4 + 2] = 30; px2[i * 4 + 3] = 255;
    }
    enc2.addFrameRGBA(px2);
}
enc2.finish();
const url2 = file2.split('\\').join('/');

viaProp.setAttribute('src', url2);
assert(viaProp.getAttribute('src') === url2, 'the attribute took the new value');
flush();
assert(viaProp.currentSrc.indexOf('bro_drift_setattr2') >= 0,
       'setAttribute("src") opened the new file: ' + viaProp.currentSrc);
assert(viaProp.videoWidth === 32,
       'and the element reports the NEW clip size: ' + viaProp.videoWidth);
assert(viaProp.videoHeight === 48,
       'both dimensions: ' + viaProp.videoHeight);

viaProp.remove();
root.innerHTML = '';
try { fs.unlinkSync(file); } catch (e) {}
try { fs.unlinkSync(file2); } catch (e) {}
