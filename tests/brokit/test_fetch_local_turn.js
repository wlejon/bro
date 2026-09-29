// A local fetch (file, data:, blob:) settles as the task right after the one
// that made it, not at the next frame's brokit pump: a page that fetches its
// templates at boot has them before its first frame, as it would from a
// browser's disk cache. Counted in rAF callbacks, which run once per frame.

let frames = 0;
requestAnimationFrame(function tick() { frames++; requestAnimationFrame(tick); });

const res = await fetch('index.html');
assert(res.ok, 'local fetch ok: ' + res.status);
const text = await res.text();
assert(text.length > 0, 'local fetch has a body');
assert(frames === 0, 'local fetch settled before the first frame (frames: ' + frames + ')');

const data = await (await fetch('data:text/plain,same-turn')).text();
assert(data === 'same-turn', 'data: fetch body: ' + data);
assert(frames === 0, 'data: fetch settled before the first frame (frames: ' + frames + ')');

// A chain of dependent local fetches still resolves within the turn.
let chained = 0;
for (let i = 0; i < 5; i++) {
    const r = await fetch('index.html');
    if ((await r.text()).length > 0) chained++;
}
assert(chained === 5, 'chained local fetches: ' + chained);
assert(frames === 0, 'chained local fetches settled before the first frame (frames: ' + frames + ')');

// An abort in the same turn still wins over the parked response.
const ac = new AbortController();
const aborted = fetch('index.html', { signal: ac.signal }).then(() => 'resolved', (e) => e.name);
ac.abort();
assert((await aborted) === 'AbortError', 'same-turn abort rejects the local fetch');

// Frames still run afterwards.
advanceTime(50);
flush();
assert(frames > 0, 'frames run after the fetches (frames: ' + frames + ')');
