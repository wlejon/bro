// bro.media.tags(path): a music file's tags, length and format from
// broaudio's tag reader, on the page and in a Worker. Fixtures are built here
// as bytes (an ID3v2.4 MP3, a FLAC with a PICTURE block, an Ogg Opus with a
// base64 cover, a WAV with LIST/INFO) and written to a scratch folder; the
// M4A is a real encoder's file (tests/audio/fixtures/quiet_loud.m4a, ffmpeg
// AAC with iTunes-style tags). broaudio's tests/test_audio_tags.cpp covers
// the formats in depth; this is the binding's shape and its worker.

const fs = require('fs');
const os = require('os');
const path = require('path');

const dir = path.join(os.tmpdir(), 'bro_media_tags_' + Date.now() + '_' + Math.floor(Math.random() * 1e6));
fs.mkdirSync(dir, { recursive: true });
const put = (name, bytes) => { const p = path.join(dir, name); fs.writeFileSync(p, bytes); return p; };

const cat = (...parts) => {
    const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
    let o = 0;
    for (const p of parts) { out.set(p, o); o += p.length; }
    return out;
};
const utf8 = (s) => new TextEncoder().encode(s);
const ascii = (s) => Uint8Array.from(s, (c) => c.charCodeAt(0));
const be32 = (n) => Uint8Array.of(n >>> 24, (n >> 16) & 255, (n >> 8) & 255, n & 255);
const le32 = (n) => Uint8Array.of(n & 255, (n >> 8) & 255, (n >> 16) & 255, n >>> 24);
const ss = (n) => Uint8Array.of((n >> 21) & 127, (n >> 14) & 127, (n >> 7) & 127, n & 127);
const png = Uint8Array.of(0x89, 0x50, 0x4e, 0x47, 13, 10, 26, 10, 1, 2, 3, 4, 5, 6, 7, 8);
const same = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);

// ---- MP3: ID3v2.4 (UTF-16 title, UTF-8 artist), APIC, eight CBR frames --------------------------
function frame(id, enc, body) {
    const data = cat(Uint8Array.of(enc), body);
    return cat(ascii(id), ss(data.length), Uint8Array.of(0, 0), data);
}
const utf16bom = (s) => { const out = [0xff, 0xfe]; for (const ch of s) { const c = ch.charCodeAt(0); out.push(c & 255, c >> 8); } return Uint8Array.from(out); };
const apicData = cat(Uint8Array.of(0), ascii('image/png'), Uint8Array.of(0, 3), ascii('cover'), Uint8Array.of(0), png);
const apic = cat(ascii('APIC'), ss(apicData.length), Uint8Array.of(0, 0), apicData);
const id3body = cat(frame('TIT2', 1, utf16bom('Ünïcode Söng')), frame('TPE1', 3, utf8('Björk')),
                    frame('TALB', 0, ascii('Album')), frame('TRCK', 0, ascii('3/12')), frame('TPOS', 0, ascii('2/2')),
                    frame('TDRC', 3, ascii('2021-05-03')), frame('TCON', 0, ascii('Jazz')), apic, new Uint8Array(32));
const mp3Frame = new Uint8Array(417);
mp3Frame.set([0xff, 0xfb, 0x90, 0x64]);
const mp3 = put('song.mp3', cat(ascii('ID3'), Uint8Array.of(4, 0, 0), ss(id3body.length), id3body,
                                ...Array.from({ length: 8 }, () => mp3Frame)));

// ---- FLAC: STREAMINFO (125.5 s at 48 kHz) + VORBIS_COMMENT + PICTURE ----------------------------
const vorbisComment = (fields) => {
    const entries = Object.entries(fields).map(([k, v]) => utf8(`${k}=${v}`));
    return cat(le32(4), ascii('test'), le32(entries.length), ...entries.map((e) => cat(le32(e.length), e)));
};
const pictureBlock = cat(be32(3), be32(9), ascii('image/png'), be32(0), be32(1), be32(1), be32(24), be32(0), be32(png.length), png);
const si = new Uint8Array(34);
si[10] = (48000 >> 12) & 255; si[11] = (48000 >> 4) & 255; si[12] = ((48000 & 15) << 4) | (1 << 1);
si[13] = 0xf0; si.set(be32(Math.round(125.5 * 48000)), 14);
const block = (type, last, body) => cat(Uint8Array.of((last ? 0x80 : 0) | type, body.length >> 16, (body.length >> 8) & 255, body.length & 255), body);
const vc = vorbisComment({ TITLE: 'Flüte Song', ARTIST: 'A', Artist: 'B', ALBUM: 'Ünder Water', TRACKNUMBER: '4', TRACKTOTAL: '9', DATE: '2019' });
const flac = put('song.flac', cat(ascii('fLaC'), block(0, false, si), block(4, false, vc), block(6, true, pictureBlock)));

// ---- Ogg Opus: comments with a base64 METADATA_BLOCK_PICTURE; 2 s after the pre-skip -----------
const b64 = (bytes) => btoa(String.fromCharCode(...bytes));
function oggPage(serial, seq, granule, packets, flags) {
    const lacing = [];
    for (const p of packets) { let n = p.length; while (n >= 255) { lacing.push(255); n -= 255; } lacing.push(n); }
    return cat(ascii('OggS'), Uint8Array.of(0, flags), le32(granule), le32(0), le32(serial), le32(seq), le32(0),
               Uint8Array.of(lacing.length), Uint8Array.from(lacing), ...packets);
}
const opusHead = cat(ascii('OpusHead'), Uint8Array.of(1, 2, 0x38, 0x01), le32(48000), Uint8Array.of(0, 0, 0));
const opusTags = cat(ascii('OpusTags'), vorbisComment({ TITLE: 'Opus Title', ARTIST: 'Opus Artist', TRACKNUMBER: '5/6',
                                                         METADATA_BLOCK_PICTURE: b64(pictureBlock) }));
const opus = put('song.opus', cat(oggPage(9, 0, 0, [opusHead], 2), oggPage(9, 1, 0, [opusTags], 0),
                                  oggPage(9, 2, 48000 * 2 + 312, [new Uint8Array(20)], 4)));

// ---- WAV: LIST/INFO, 0.25 s mono 16-bit at 8 kHz ------------------------------------------------
const chunk = (id, body) => cat(ascii(id), le32(body.length), body, body.length & 1 ? Uint8Array.of(0) : new Uint8Array(0));
const fmt = cat(Uint8Array.of(1, 0, 1, 0), le32(8000), le32(16000), Uint8Array.of(2, 0, 16, 0));
const info = cat(ascii('INFO'), chunk('INAM', ascii('Wav Title\0')), chunk('IART', ascii('Wav Artist\0')), chunk('ITRK', ascii('11\0')));
const wavBody = cat(ascii('WAVE'), chunk('fmt ', fmt), chunk('LIST', info), chunk('data', new Uint8Array(4000)));
const wav = put('song.wav', cat(ascii('RIFF'), le32(wavBody.length), wavBody));

const M4A = path.resolve('tests/audio/fixtures/quiet_loud.m4a');
const KEYS = ['title', 'artist', 'album', 'albumArtist', 'track', 'trackTotal', 'disc', 'discTotal', 'year', 'genre',
              'duration', 'sampleRate', 'channels', 'bitrate', 'codec', 'picture'];

// The checks, as one function so the worker's answers go through the same ones.
function check(where, r) {
    const { mp3: a, flac: f, opus: o, wav: w, m4a: m } = r;
    for (const k of KEYS) assert(k in a, `${where}: tags() has ${k}`);
    assert(a.title === 'Ünïcode Söng' && a.artist === 'Björk' && a.album === 'Album', `${where}: MP3 text: ${a.title} / ${a.artist}`);
    assert(a.track === 3 && a.trackTotal === 12 && a.disc === 2 && a.discTotal === 2 && a.year === 2021 && a.genre === 'Jazz',
           `${where}: MP3 numbers`);
    assert(a.codec === 'mp3' && a.sampleRate === 44100 && a.channels === 2 && a.bitrate === 128000, `${where}: MP3 format`);
    assert(Math.abs(a.duration - (8 * 417 * 8) / 128000) < 1e-6, `${where}: MP3 CBR length ${a.duration}`);
    assert(a.picture && a.picture.mime === 'image/png' && a.picture.bytes instanceof Uint8Array && same(a.picture.bytes, png),
           `${where}: MP3 cover`);

    assert(f.title === 'Flüte Song' && f.artist === 'A, B' && f.album === 'Ünder Water', `${where}: FLAC text ${f.artist}`);
    assert(f.track === 4 && f.trackTotal === 9 && f.year === 2019, `${where}: FLAC numbers`);
    assert(f.codec === 'flac' && f.sampleRate === 48000 && f.channels === 2 && Math.abs(f.duration - 125.5) < 1e-6, `${where}: FLAC STREAMINFO`);
    assert(f.picture && same(f.picture.bytes, png), `${where}: FLAC PICTURE`);

    assert(o.codec === 'opus' && o.title === 'Opus Title' && o.track === 5 && o.trackTotal === 6, `${where}: Opus comments`);
    assert(Math.abs(o.duration - 2) < 1e-6, `${where}: Opus length less its pre-skip: ${o.duration}`);
    assert(o.picture && o.picture.mime === 'image/png' && same(o.picture.bytes, png), `${where}: Opus base64 cover`);

    assert(w.codec === 'pcm' && w.title === 'Wav Title' && w.artist === 'Wav Artist' && w.track === 11, `${where}: WAV INFO`);
    assert(Math.abs(w.duration - 0.25) < 1e-6 && w.sampleRate === 8000 && w.channels === 1, `${where}: WAV length`);
    assert(w.album === '' && w.year === 0 && w.picture === null, `${where}: absent fields are '' / 0 / null`);

    assert(m.codec === 'aac' && m.title === 'Quiet Loud' && m.artist === 'bro tests' && m.album === 'Fixtures', `${where}: M4A ilst`);
    assert(m.track === 7 && m.trackTotal === 9 && m.year === 2026, `${where}: M4A trkn / ©day`);
    assert(m.sampleRate === 44100 && m.channels === 1 && Math.abs(m.duration - 2) < 0.001, `${where}: M4A length ${m.duration}`);
    assert(m.bitrate > 20000, `${where}: M4A bit rate ${m.bitrate}`);

    assert(r.missing === null && r.junk === null, `${where}: a missing or unrecognised file is null`);
}

const junk = put('junk.mp3', new Uint8Array(64));
const files = { mp3, flac, opus, wav, m4a: M4A, missing: path.join(dir, 'nope.mp3'), junk };

// ---- on the page --------------------------------------------------------------------------------
assert(typeof bro.media.tags === 'function', 'bro.media.tags exists');
let threw = false;
try { bro.media.tags(); } catch (e) { threw = e instanceof TypeError; }
assert(threw, 'tags() with no path is a TypeError');
const here = {};
for (const [k, p] of Object.entries(files)) here[k] = bro.media.tags(p);
check('page', here);

// ---- in a Worker --------------------------------------------------------------------------------
const w = new Worker('../video/worker_media_tags.js');
let got = null;
w.onmessage = (e) => { got = e.data; };
w.postMessage(files);
const deadline = Date.now() + 15000;
while (got === null && Date.now() < deadline) { advanceTime(16); wallSleep(2); }
assert(got !== null, 'the worker answered');
assert(!got.error, 'the worker read the tags: ' + got.error);
check('worker', got);
w.terminate();

for (const f of fs.readdirSync(dir)) fs.unlinkSync(path.join(dir, f));
fs.rmdirSync(dir);
console.log('test_media_tags PASSED');
