// The window scrolls the viewport (the root scroller), and client rects are
// taken in the scrolled viewport: an element's getBoundingClientRect agrees
// with a Range over its contents and with elementFromPoint once the document
// has scrolled. window.scrollTo used to set <html>'s scrollTop (a no-op when
// <html> is not a scroller) while window.scrollY read the viewport, and
// element rects ignored the viewport's scroll while Range rects took it off.

const root = document.getElementById('root');
document.body.style.height = 'auto';
root.innerHTML =
    '<div id="top" style="height:200px">top</div>' +
    '<div id="mark" style="height:40px;font:16px monospace">marker text</div>' +
    '<div style="height:5000px"></div>';
flush();

const mark = document.getElementById('mark');
const at0 = mark.getBoundingClientRect().top;
assert(Math.abs(at0 - 200) < 1, 'unscrolled, the marker is at 200, got ' + at0);

let scrolls = 0;
document.documentElement.addEventListener('scroll', () => ++scrolls);

window.scrollTo(0, 150);
assert(Math.abs(window.scrollY - 150) < 0.5, 'scrollTo moves the viewport, scrollY ' + window.scrollY);
assert(Math.abs(document.documentElement.scrollTop - 150) < 0.5,
       '<html>.scrollTop reads the viewport, got ' + document.documentElement.scrollTop);
assert(scrolls === 1, 'one scroll event, got ' + scrolls);
flush();

const r = mark.getBoundingClientRect();
assert(Math.abs(r.top - 50) < 1, 'the element rect takes the scroll off, got ' + r.top);
assert(Math.abs(mark.getClientRects()[0].top - 50) < 1, 'so do its client rects');
const range = document.createRange();
range.selectNodeContents(mark);
const rr = range.getBoundingClientRect();
assert(Math.abs(rr.top - r.top) < 2, 'a Range over it agrees: ' + rr.top + ' vs ' + r.top);
const hit = document.elementFromPoint(r.left + 5, r.top + 5);
assert(hit === mark, 'elementFromPoint at its rect finds it, got ' + (hit && hit.id));

window.scrollBy(0, 30);
assert(Math.abs(window.scrollY - 180) < 0.5, 'scrollBy is relative, got ' + window.scrollY);
window.scrollTo({ left: 0 });
assert(Math.abs(window.scrollY - 180) < 0.5, 'an options object without top keeps it, got ' + window.scrollY);

document.documentElement.scrollTop = 20;
assert(Math.abs(window.scrollY - 20) < 0.5, 'writing <html>.scrollTop scrolls the viewport, got ' + window.scrollY);

window.scrollTo(0, 1e9);
const max = window.scrollY;
assert(max > 4000 && max < 6000, 'scrollTo clamps to the scrollable range, got ' + max);

// scrollIntoView reaches the viewport too.
window.scrollTo(0, 0);
mark.scrollIntoView();
assert(Math.abs(window.scrollY - 200) < 1, 'scrollIntoView scrolls the viewport, got ' + window.scrollY);
assert(Math.abs(mark.getBoundingClientRect().top) < 1, 'and brings the element to the top');

window.scrollTo(0, 0);
document.body.style.height = '';
root.innerHTML = '';
