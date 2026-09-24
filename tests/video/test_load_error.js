// The media element load algorithm on a source that cannot be opened.
//
// load() starts over from HAVE_NOTHING: the previous resource is dropped
// (`emptied`), and a source that is missing or in a format bro cannot read
// fires `error` with error.code MEDIA_ERR_SRC_NOT_SUPPORTED. The element used
// to fire nothing and keep describing the previous file (readyState 4, its
// duration, its videoWidth), so a page waiting on either event hung.

const os = require('os');
const path = require('path');
const fs = require('fs');

const file = path.join(os.tmpdir(), 'bro_load_error_' + Date.now() + '.webm');
const W = 48, H = 32;
const enc = new VideoEncoder({ path: file, width: W, height: H,
                               fps: 5, fpsDen: 1, quality: 'realtime' });
const px = new Uint8Array(W * H * 4).fill(200);
for (let f = 0; f < 5; f++) enc.addFrameRGBA(px);
enc.finish();
const url = file.split('\\').join('/');

// Not a WebM: some bytes in a file with an .ogg name.
const bogus = path.join(os.tmpdir(), 'bro_load_error_' + Date.now() + '.ogg');
fs.writeFileSync(bogus, 'OggS this is not a media file bro can demux');
const bogusUrl = bogus.split('\\').join('/');

const v = document.createElement('video');
document.getElementById('root').appendChild(v);
const seen = [];
for (const t of ['emptied', 'loadedmetadata', 'error'])
    v.addEventListener(t, () => seen.push(t));

function pumpUntil(pred) {
    const t = Date.now();
    while (!pred() && Date.now() - t < 10000) { sleep(10); flush(); advanceTime(20); }
    return pred();
}

v.src = url;
v.load();
assert(pumpUntil(() => seen.includes('loadedmetadata')), 'the good clip loads: ' + seen.join(','));
assert(v.readyState === 4 && v.videoWidth === W, 'and describes itself');
assert(v.error === null, 'no error for a good clip');

for (const bad of [bogusUrl, url + '.missing.webm']) {
    seen.length = 0;
    v.src = bad;
    v.load();
    assert(v.readyState === 0, 'load() resets readyState to HAVE_NOTHING (' + v.readyState + ')');
    assert(Number.isNaN(v.duration), 'duration is NaN with nothing loaded (' + v.duration + ')');
    assert(v.videoWidth === 0 && v.videoHeight === 0, 'no picture size (' + v.videoWidth + ')');
    assert(pumpUntil(() => seen.includes('error')), 'error fires for ' + bad + ': ' + seen.join(','));
    assert(seen[0] === 'emptied', 'emptied comes first: ' + seen.join(','));
    assert(!seen.includes('loadedmetadata'), 'no loadedmetadata');
    assert(v.error && v.error.code === 4 && v.error.code === v.error.MEDIA_ERR_SRC_NOT_SUPPORTED,
           'error.code is MEDIA_ERR_SRC_NOT_SUPPORTED');
    assert(typeof v.error.message === 'string' && v.error.message.length > 0, 'with a message');
    assert(v.networkState === 3, 'networkState NETWORK_NO_SOURCE (' + v.networkState + ')');
    let rejected = false;
    v.play().catch(() => { rejected = true; });
    pumpUntil(() => rejected);
    assert(rejected, 'play() rejects');
    assert(seen.filter((t) => t === 'error').length === 1, 'and does not fire a second error');
}

// A good source after a bad one recovers and clears the error.
seen.length = 0;
v.src = url;
assert(pumpUntil(() => seen.includes('loadedmetadata')), 'recovers: ' + seen.join(','));
assert(v.error === null && v.readyState === 4 && v.videoWidth === W, 'error cleared, state back');

try { fs.unlinkSync(file); fs.unlinkSync(bogus); } catch (e) {}
console.log('PASS');
