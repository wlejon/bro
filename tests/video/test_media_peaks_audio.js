// bro.media.peaks on audio-only files: WAV, FLAC, MP3, Ogg Vorbis and Ogg
// Opus decode through broaudio's streaming decoders (a chunk at a time, never
// the whole file), with the same result shape and `from`/`to` window as the
// WebM path in test_media_analysis.js.
//
// Every fixture is the same 2 s mono signal — silent for 1 s, then 440 Hz at
// 0.6 — so "did the analysis see the file" has one answer per format.

const os = require('os');
const path = require('path');

const FIX = 'tests/audio/fixtures';
const files = [
    { name: 'flac', file: path.resolve(FIX, 'quiet_loud.flac'), rate: 22050 },
    { name: 'mp3', file: path.resolve(FIX, 'quiet_loud.mp3'), rate: 44100, slack: 0.06 },
    { name: 'vorbis', file: path.resolve(FIX, 'quiet_loud.ogg'), rate: 22050 },
    { name: 'opus', file: path.resolve(FIX, 'quiet_loud.opus'), rate: 48000 },
];

// WAV straight from the engine's own writer.
{
    const ctx = new AudioContext();
    const rate = 32000;
    const pcm = new Float32Array(rate * 2);
    for (let i = rate; i < pcm.length; i++) pcm[i] = 0.6 * Math.sin(2 * Math.PI * 440 * i / rate);
    const wav = path.join(os.tmpdir(), 'bro_media_peaks_' + Date.now() + '.wav');
    assert(ctx.saveWav(wav, pcm, 1, rate), 'wrote the WAV fixture');
    files.push({ name: 'wav', file: wav, rate });
}

for (const f of files) {
    const p = bro.media.peaks(f.file, { buckets: 100 });
    assert(p, f.name + ': peaks() returned a result');
    assert(p.sampleRate === f.rate, f.name + ': sampleRate ' + p.sampleRate + ' = ' + f.rate);
    assert(p.channels === 1, f.name + ': channels ' + p.channels);
    assert(Math.abs(p.duration - 2) < (f.slack || 0.01), f.name + ': duration ' + p.duration);
    assert(p.buckets === 100 && p.min.length === 100 && p.max.length === 100 && p.rms.length === 100,
           f.name + ': one entry per bucket');
    assert(p.from === 0 && Math.abs(p.to - p.duration) < 1e-9, f.name + ': covers the whole file');

    let quiet = 0, loud = 0, peak = 0;
    for (let i = 0; i < 48; ++i) if (p.rms[i] < 0.02) quiet++;
    for (let i = 53; i < 97; ++i) if (p.rms[i] > 0.35 && p.rms[i] < 0.5) loud++;
    for (let i = 0; i < 100; ++i) peak = Math.max(peak, p.max[i], -p.min[i]);
    assert(quiet >= 46, f.name + ': first half reads quiet (' + quiet + '/48)');
    assert(loud >= 42, f.name + ': second half reads loud (' + loud + '/44)');
    assert(peak > 0.55 && peak < 0.7, f.name + ': envelope peaks at the 0.6 amplitude, ' + peak);

    // A window: only the loud second.
    const w = bro.media.peaks(f.file, { buckets: 20, from: 1.2, to: 1.8 });
    assert(w, f.name + ': windowed peaks');
    assert(Math.abs(w.from - 1.2) < 1e-6 && Math.abs(w.to - 1.8) < 1e-6, f.name + ': window reported, ' + w.from + '..' + w.to);
    assert(Math.abs(w.duration - p.duration) < 1e-9, f.name + ': duration is still the file\'s');
    let wl = 0;
    for (let i = 0; i < 20; ++i) if (w.rms[i] > 0.35) wl++;
    assert(wl === 20, f.name + ': every bucket of the loud window is loud (' + wl + '/20)');

    // And the quiet one.
    const q = bro.media.peaks(f.file, { buckets: 10, to: 0.9 });
    let qq = 0;
    for (let i = 0; i < 10; ++i) if (q.rms[i] < 0.02) qq++;
    assert(qq === 10, f.name + ': every bucket of the quiet window is quiet (' + qq + '/10)');

    // An empty window is a refusal, as on the WebM path.
    assert(bro.media.peaks(f.file, { from: 5 }) === null, f.name + ': a window past the end is null');
}

assert(bro.media.peaks(path.resolve(FIX, 'does_not_exist.mp3')) === null, 'a missing file is null');

console.log('media peaks (audio-only) tests passed');
