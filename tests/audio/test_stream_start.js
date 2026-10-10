// A disk stream is playing from the moment createStreamFromFile returns, even
// while its worker is still decoding the prebuffer (the mixer holds it silent
// until then): isClipPlaying is true in that window, so a player that ends a
// song on `!isClipPlaying` does not end it before it starts. And a pause in
// that window sticks: the worker's release does not un-pause the stream.

const path = require('path');
const FILE = path.resolve('tests/audio/fixtures/quiet_loud.flac');

const ctx = new AudioContext();

function realWait(ms) {
    const t0 = Date.now();
    while (Date.now() - t0 < ms) {}
}

// Right after the call, many times over: the worker has rarely finished its
// prebuffer by then, so a stream reported as paused while buffering would
// show here.
let early = 0;
for (let i = 0; i < 20; i++) {
    const pb = ctx.createStreamFromFile(FILE, { prebufferFrames: ctx.sampleRate });
    if (!ctx.isClipPlaying(pb)) early++;
    ctx.closeStream(pb);
}
assert(early === 0, early + ' of 20 streams read as not playing while they buffered');

// Paused while buffering: stays paused once the prebuffer is in.
const pb = ctx.createStreamFromFile(FILE, { prebufferFrames: ctx.sampleRate });
ctx.setPlaybackPlaying(pb, false);
assert(!ctx.isClipPlaying(pb), 'paused at once');
const t0 = Date.now();
while (Date.now() - t0 < 3000) {
    const st = ctx.getStreamStats(pb);
    if (st && (st.finished || st.bufferedFrames >= ctx.sampleRate / 2)) break;
    realWait(5);
}
realWait(50);
sleep(200);
assert(!ctx.isClipPlaying(pb), 'still paused after the worker decoded the prebuffer');
assert(ctx.getPlaybackPositionSeconds(pb) < 0.01,
       'a stream paused while buffering has not moved: ' + ctx.getPlaybackPositionSeconds(pb));

// And it plays when asked.
ctx.setPlaybackPlaying(pb, true);
assert(ctx.isClipPlaying(pb), 'plays after the pause');
sleep(200);
assert(ctx.getPlaybackPositionSeconds(pb) > 0.1, 'and moves: ' + ctx.getPlaybackPositionSeconds(pb));
ctx.closeStream(pb);
