// reverse(), and updatePlaybackRate() on a running animation, set a PENDING
// playback rate (Web Animations "reverse an animation", "seamlessly update
// the playback rate"), as Chromium has it: `playbackRate` still answers the
// old rate, `pending` is true, the current time holds, the play state is
// running, and the next frame applies the rate and resolves `ready`.
// Setting `playbackRate` applies at once. bro read -1 straight after
// reverse() (the waapi survey page).

const root = document.getElementById('root');
root.innerHTML = '<div id="box" style="width:100px;height:20px"></div>';
const box = document.getElementById('box');

const b = box.animate({ width: ['100px', '300px'] }, { duration: 200, iterations: 2, direction: 'alternate' });
assert(b.pending === false, 'a fresh animation is not pending here');
b.reverse();
assert(b.playbackRate === 1, 'playbackRate answers the old rate until ready, got ' + b.playbackRate);
assert(b.pending === true, 'reverse() leaves the animation pending');
assert(b.currentTime === 400, 'reversed from the start it plays from the end, got ' + b.currentTime);
assert(b.playState === 'running', 'held at its end with a pending -1 it is running, got ' + b.playState);
let readyRate = null;
b.ready.then((x) => { readyRate = x === b ? b.playbackRate : 'wrong'; });
advanceTime(16);
assert(b.playbackRate === -1, 'the frame applies the rate');
// ready is delivered with the animation events, on the frame after the tick.
advanceTime(16);
assert(readyRate === -1, 'ready resolves with the rate applied, got ' + readyRate);
assert(b.pending === false, 'not pending after the frame');
advanceTime(100);
assert(b.currentTime > 250 && b.currentTime < 400, 'it then runs backwards, got ' + b.currentTime);

// updatePlaybackRate on a running animation: pending, no jump.
const c = box.animate({ opacity: [1, 0] }, { duration: 1000 });
advanceTime(100);
const before = c.currentTime;
c.updatePlaybackRate(2);
assert(c.playbackRate === 1 && c.pending === true, 'updatePlaybackRate is pending while running');
assert(c.currentTime === before, 'the current time holds');
advanceTime(16);
assert(c.playbackRate === 2 && c.pending === false, 'applied on the next frame');
advanceTime(100);
assert(Math.abs(c.currentTime - (before + 200)) < 1, 'runs at the new rate from where it was, got ' + c.currentTime);

// Setting playbackRate applies at once and drops a pending rate.
c.updatePlaybackRate(3);
c.playbackRate = 0.5;
assert(c.playbackRate === 0.5 && c.pending === false, 'playbackRate = x applies at once');
advanceTime(16);
assert(c.playbackRate === 0.5, 'the dropped pending rate is not applied later');

// On a paused animation updatePlaybackRate applies at once.
const d = box.animate({ opacity: [1, 0] }, { duration: 1000 });
d.pause();
d.updatePlaybackRate(-1);
assert(d.playbackRate === -1 && d.pending === false, 'paused: applied at once');
b.cancel(); c.cancel(); d.cancel();
