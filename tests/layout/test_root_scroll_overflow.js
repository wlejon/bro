// The root scroller's range covers content that overflows <html>: a body
// sized to the window with a taller child can be scrolled to the end of that
// child. The range used to be <html>'s own box, so overflowing content was
// out of reach. Content an overflow clip hides, and fixed boxes (they sit in
// the viewport, not the page), do not extend it.

const root = document.getElementById('root');
// test_app's body is 1920x1080, the viewport's size: nothing to scroll by
// <html>'s box alone.
root.innerHTML =
    '<div style="height:3000px">tall</div>' +
    '<div style="height:100px;overflow:hidden"><div style="height:9000px"></div></div>' +
    '<div style="position:fixed;top:0;height:20000px;width:10px"></div>';
flush();

const vh = window.innerHeight;
window.scrollTo(0, 1e9);
const max = window.scrollY;
assert(Math.abs(max - (3100 - vh)) < 2,
       'the range ends at the overflowing content (' + (3100 - vh) + '), got ' + max);

// Wheel scrolling uses the same range.
window.scrollTo(0, 0);
for (let i = 0; i < 40; i++) {
    wheel(500, 500, 30);
    advanceTime(50);
    flush();
}
for (let i = 0; i < 20; i++) { advanceTime(50); flush(); }
assert(Math.abs(window.scrollY - max) < 2, 'the wheel reaches the same end, got ' + window.scrollY);

window.scrollTo(0, 0);
root.innerHTML = '';
flush();
window.scrollTo(0, 1e9);
assert(window.scrollY === 0, 'with nothing overflowing there is nothing to scroll, got ' + window.scrollY);
