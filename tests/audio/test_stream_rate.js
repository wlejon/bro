// setPlaybackRate on a LIVE PCM stream (createStream).
//
// This used to be a documented no-op — the ring mixer read whole frames with
// an integer cursor, so a stream ignored rate entirely. The symptom that
// found it: a video player at 0.25x had slow-motion picture over full-speed
// sound. A stream at rate r must drain its ring r times as fast.

const ctx = new AudioContext();
const RATE = ctx.sampleRate;

// Real device: the mixer has to actually consume the ring for played frames
// to move. Run this with `bro-headless --audio`.
if (!ctx.sampleRate) {
    console.log('SKIP: no audio context');
} else {
    const consumed = (rate) => {
        const s = ctx.createStream(1, RATE * 4);
        ctx.setPlaybackRate(s, rate);
        // More audio than the measurement window will consume, so a shortfall
        // is a rate bug and not an underrun.
        const n = RATE * 3;
        const pcm = new Float32Array(n);
        for (let i = 0; i < n; i++) pcm[i] = 0.05 * Math.sin(i * 0.05);
        ctx.pushStreamSamples(s, pcm);

        const before = ctx.getStreamStats(s).playedFrames;
        wallSleep(700);
        const after = ctx.getStreamStats(s).playedFrames;
        ctx.closeStream(s);
        return after - before;
    };

    const base = consumed(1.0);
    if (base < 1000) {
        // No device attached (the default headless configuration): the mixer
        // never runs, so there is nothing to measure. Not a failure.
        console.log('SKIP: mixer is not consuming (run with --audio)');
    } else {
        const slow = consumed(0.25);
        const fast = consumed(2.0);
        console.log(`rate 1.00: ${base} frames`);
        console.log(`rate 0.25: ${slow} frames (${(slow / base).toFixed(3)}x)`);
        console.log(`rate 2.00: ${fast} frames (${(fast / base).toFixed(3)}x)`);

        assert(slow / base > 0.15 && slow / base < 0.40,
               `rate 0.25 should consume ~a quarter as fast, got ${(slow / base).toFixed(3)}`);
        assert(fast / base > 1.6 && fast / base < 2.4,
               `rate 2.0 should consume ~twice as fast, got ${(fast / base).toFixed(3)}`);
    }
}

console.log('PASS');
