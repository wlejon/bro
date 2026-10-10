// bro.media.canDecode(type): whether bro decodes a file of `type` on this
// machine, answered as HTMLMediaElement.canPlayType answers ('' | 'maybe' |
// 'probably'). M4A/AAC goes through the platform's AAC decoder (Media
// Foundation, AudioToolbox), which a Linux build does not have. And
// <input accept="audio/*">'s native picker offers what bro decodes,
// M4A/M4B included.

const os = require('os');

assert(bro.media && typeof bro.media.canDecode === 'function', 'bro.media.canDecode exists');
const cd = bro.media.canDecode;

for (const t of ['audio/wav', 'audio/mpeg', 'audio/flac', '.wav', 'mp3', 'FLAC', 'audio/ogg; codecs="vorbis"'])
    assert(cd(t) === 'probably', t + ' decodes: ' + cd(t));
for (const t of ['video/webm', 'audio/x-nope', 'xyz', '', 'audio/mp4; codecs="ac-3"', 'audio/mpeg; codecs="opus"'])
    assert(cd(t) === '', t + ' does not: "' + cd(t) + '"');

// AAC: where the platform decodes it (and test_m4a_decode.js proves it does).
const aac = os.platform() !== 'linux';
const want = aac ? 'probably' : '';
for (const t of ['.m4a', 'm4b', 'audio/mp4; codecs="mp4a.40.2"', 'audio/x-m4a; codecs=mp4a.40.5'])
    assert(cd(t) === want, t + ': "' + cd(t) + '", want "' + want + '"');
assert(cd('audio/mp4') === (aac ? 'maybe' : ''), 'a bare audio/mp4 may hold more than AAC: "' + cd('audio/mp4') + '"');
let threw = false;
try { cd(); } catch (e) { threw = e instanceof TypeError; }
assert(threw, 'canDecode() with no type throws');

// The picker's filter for audio/*.
const root = document.getElementById('root');
root.innerHTML = '<input id="f" type="file" accept="audio/*">';
flush();
setPickedFiles([]);
document.getElementById('f').click();
const filter = lastFileDialogFilter();
const exts = (filter.split('|')[1] || '').split(';');
for (const e of ['wav', 'mp3', 'flac', 'ogg', 'opus', 'm4a', 'm4b'])
    assert(exts.includes(e), 'audio/* offers .' + e + ': ' + filter);
console.log('test_media_can_decode.js PASSED');
