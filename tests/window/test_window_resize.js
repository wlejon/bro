// bro.window.getSize / setSize, window.resizeTo / resizeBy, bro.quit.
// Headless, setSize resizes the virtual viewport (the resize() helper's
// path): innerWidth/innerHeight follow and 'resize' fires on window.

assert(typeof bro.window.getSize === 'function', 'bro.window.getSize exists');
assert(typeof bro.window.setSize === 'function', 'bro.window.setSize exists');
assert(typeof window.resizeTo === 'function', 'window.resizeTo exists');
assert(typeof window.resizeBy === 'function', 'window.resizeBy exists');
assert(typeof bro.quit === 'function', 'bro.quit exists');

const s0 = bro.window.getSize();
assert(s0.width === 1920 && s0.height === 1080,
       'headless starts 1920x1080, got ' + s0.width + 'x' + s0.height);

let resizes = 0;
window.addEventListener('resize', () => { resizes++; });

bro.window.setSize(800, 600);
let s = bro.window.getSize();
assert(s.width === 800 && s.height === 600, 'setSize round-trips: ' + s.width + 'x' + s.height);
assert(window.innerWidth === 800 && window.innerHeight === 600,
       'innerWidth/innerHeight follow: ' + window.innerWidth + 'x' + window.innerHeight);
assert(resizes === 1, 'setSize fires one resize event, got ' + resizes);

window.resizeTo(1024, 768);
s = bro.window.getSize();
assert(s.width === 1024 && s.height === 768, 'resizeTo: ' + s.width + 'x' + s.height);

window.resizeBy(-24, 32);
s = bro.window.getSize();
assert(s.width === 1000 && s.height === 800, 'resizeBy: ' + s.width + 'x' + s.height);
assert(resizes === 3, 'each resize fires, got ' + resizes);

let threw = null;
try { bro.window.setSize(0, 100); } catch (e) { threw = e; }
assert(threw instanceof RangeError, 'setSize(0, h) throws RangeError');
threw = null;
try { bro.window.setSize(); } catch (e) { threw = e; }
assert(threw instanceof TypeError, 'setSize() throws TypeError');
s = bro.window.getSize();
assert(s.width === 1000 && s.height === 800, 'a rejected setSize leaves the size alone');

// Layout follows: a 100%-wide block measures the new width.
const d = document.createElement('div');
d.style.width = '100%';
d.style.height = '10px';
document.body.style.margin = '0';
document.body.style.width = 'auto';  // the test app pins body to 1920px
document.body.appendChild(d);
assert(Math.round(d.getBoundingClientRect().width) === 1000,
       'layout uses the new width, got ' + d.getBoundingClientRect().width);

// bro.quit is a no-op headless: the script keeps running.
bro.quit();
advanceTime(16);
assert(true, 'still running after bro.quit()');

bro.window.setSize(1920, 1080);
console.log('bro.window resize OK');
