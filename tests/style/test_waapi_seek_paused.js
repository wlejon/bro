// Seeking a paused Web Animation keeps it paused: writing currentTime sets
// the hold time instead of restarting the clock from the new time.

const root = document.getElementById('root');
const el = document.createElement('div');
el.style.position = 'absolute';
el.style.width = '10px';
el.style.height = '10px';
root.appendChild(el);

const a = el.animate([{ left: '0px' }, { left: '600px' }], { duration: 4000 });
advanceTime(1000);
a.pause();
a.currentTime = 0;
advanceTime(500);
flush();
assert(a.playState === 'paused', 'still paused, got ' + a.playState);
assert(a.currentTime === 0, 'held at the seeked time, got ' + a.currentTime);
assert(a.startTime === null, 'no start time while held, got ' + a.startTime);
assert(getComputedStyle(el).left === '0px', 'the effect shows the held time, got ' + getComputedStyle(el).left);

a.currentTime = 2000;
advanceTime(250);
flush();
assert(a.currentTime === 2000, 'a second seek holds too, got ' + a.currentTime);
assert(getComputedStyle(el).left === '300px', 'held halfway, got ' + getComputedStyle(el).left);

a.play();
advanceTime(1000);
flush();
assert(Math.abs(a.currentTime - 3000) < 1, 'play() resumes from the held time, got ' + a.currentTime);

// A running animation's seek still runs from there.
const b = el.animate([{ top: '0px' }, { top: '100px' }], { duration: 1000 });
b.currentTime = 500;
advanceTime(100);
assert(Math.abs(b.currentTime - 600) < 1 && b.playState === 'running',
    'a running seek keeps running, got ' + b.currentTime + ' ' + b.playState);
