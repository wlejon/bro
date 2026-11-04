// bro.media.tags reads WebM: bro's own VideoEncoder writes one (libwebm's
// mkvmuxer: SeekHead, Info with Duration, Tracks, Clusters, Cues) and the tag
// reader answers its length and audio format without decoding it, on the page
// and in a Worker. broaudio's tests/test_audio_tags_matroska.cpp covers the
// container in depth (tags, attachments, live recordings, damaged files).

const os = require('os');
const path = require('path');
const fs = require('fs');

const stamp = Date.now() + '_' + Math.floor(Math.random() * 1e6);
const W = 64, H = 48;

function encode(file, opts, frames, pcm) {
    const enc = new VideoEncoder(Object.assign({ path: file, width: W, height: H, quality: 'realtime' }, opts));
    const px = new Uint8Array(W * H * 4);
    for (let f = 0; f < frames; f++) {
        px.fill((f * 40) & 255);
        enc.addFrameRGBA(px);
    }
    if (pcm) enc.addAudioFramesPCM(pcm);
    enc.finish();
    assert(enc.lastError === '', 'encoded ' + file + ': ' + enc.lastError);
    return file;
}

// Video only: 10 frames at 10 fps is 1.0 s, its last frame included.
const video = encode(path.join(os.tmpdir(), 'bro_tags_video_' + stamp + '.webm'), { fps: 10 }, 10);
// Video and Opus: 6 frames at 30 fps (0.2 s) and 1.25 s of mono audio at
// 24 kHz; the file lasts as long as its longest track.
const pcm = new Float32Array(30000);
for (let i = 0; i < pcm.length; ++i) pcm[i] = Math.sin(2 * Math.PI * 440 * i / 24000) * 0.25;
const withAudio = encode(path.join(os.tmpdir(), 'bro_tags_audio_' + stamp + '.webm'),
                         { fps: 30, audioSampleRate: 24000, audioChannels: 1, audioBitrateKbps: 64 }, 6, pcm);

function check(where, v, a) {
    assert(v !== null && a !== null, `${where}: tags() reads a WebM (got ${v} / ${a})`);
    assert(v.container === 'webm', `${where}: container ${v.container}`);
    assert(Math.abs(v.duration - 1.0) < 0.1, `${where}: video-only duration within a frame of 1.0 s: ${v.duration}`);
    assert(v.codec === '' && v.sampleRate === 0 && v.channels === 0, `${where}: no audio track, no audio format (${v.codec})`);
    assert(v.title === '' && v.picture === null, `${where}: no tags`);

    assert(a.container === 'webm' && a.codec === 'opus', `${where}: audio codec ${a.codec}`);
    assert(a.channels === 1, `${where}: channels ${a.channels}`);
    assert(a.sampleRate === 24000 || a.sampleRate === 48000, `${where}: Opus rate ${a.sampleRate}`);
    assert(Math.abs(a.duration - 1.25) < 0.05, `${where}: duration is the audio's 1.25 s: ${a.duration}`);
}

const here = { video: bro.media.tags(video), audio: bro.media.tags(withAudio) };
check('page', here.video, here.audio);

const w = new Worker('../video/worker_media_tags.js');
let got = null;
w.onmessage = (e) => { got = e.data; };
w.postMessage({ video, audio: withAudio });
const deadline = Date.now() + 15000;
while (got === null && Date.now() < deadline) { advanceTime(16); wallSleep(2); }
assert(got !== null, 'the worker answered');
assert(!got.error, 'the worker read the tags: ' + got.error);
check('worker', got.video, got.audio);
w.terminate();

for (const f of [video, withAudio]) { try { fs.unlinkSync(f); } catch (e) {} }
console.log('test_media_tags_webm PASSED');
