// The CSSOM metrics family and the scroll setter's engine work.
//
// client*/scroll* are `long` in the CSSOM, they are 0 on a non-replaced
// inline, and clientWidth/clientHeight are content + padding (border
// excluded), while scrollHeight is the content's NATURAL height plus padding,
// floored at clientHeight. Writing scrollTop clamps to [0, max], marks the
// document dirty so the frame repaints at the new offset, and fires a trusted
// `scroll` event only when the offset actually changed.
//
// Regression: the port answered the raw layout numbers with no padding/inline
// rules, and its scrollTop setter stored the value with no clamp, no dirty
// mark and no event — a scroll container repainted at the old offset and no
// listener ever ran.

const root = document.getElementById('root');

// --- clientWidth / clientHeight / clientLeft / clientTop ------------------
// The test app's CSS is box-sizing:border-box, so width:100px is the BORDER
// box: client* must come back 100 - 2*border.
root.innerHTML =
    '<div id="box" style="width:100px;height:60px;padding:10px;' +
    'border:2px solid black;position:absolute;left:15px;top:25px;">box</div>' +
    '<span id="inl" style="padding:10px;">inline</span>';
flush();

const box = document.getElementById('box');
assert(box.offsetWidth === 100, 'offsetWidth is the border box: ' + box.offsetWidth);
assert(box.clientWidth === 96, 'clientWidth excludes the border: ' + box.clientWidth);
assert(box.clientHeight === 56, 'clientHeight excludes the border: ' + box.clientHeight);
assert(box.clientLeft === 2, 'clientLeft is the left border width: ' + box.clientLeft);
assert(box.clientTop === 2, 'clientTop is the top border width: ' + box.clientTop);
assert(box.clientWidth === Math.trunc(box.clientWidth), 'clientWidth is an integer');

const inl = document.getElementById('inl');
assert(inl.clientWidth === 0, 'a non-replaced inline has clientWidth 0');
assert(inl.clientHeight === 0, 'a non-replaced inline has clientHeight 0');
assert(inl.clientLeft === 0, 'a non-replaced inline has clientLeft 0');
assert(inl.scrollWidth === 0, 'a non-replaced inline has scrollWidth 0');
assert(inl.scrollHeight === 0, 'a non-replaced inline has scrollHeight 0');

// getClientRects(): one rect, agreeing with getBoundingClientRect.
const rects = box.getClientRects();
assert(rects.length === 1, 'getClientRects returns one rect: ' + rects.length);
const bcr = box.getBoundingClientRect();
assert(rects[0].width === bcr.width && rects[0].height === bcr.height,
       'getClientRects agrees with getBoundingClientRect');

// --- an overflow container: scrollHeight, clamp, event --------------------
root.innerHTML =
    '<div id="sc" style="width:200px;height:100px;padding:5px;overflow-y:auto;' +
    'line-height:20px;">' +
    'L1<br>L2<br>L3<br>L4<br>L5<br>L6<br>L7<br>L8<br>L9<br>L10<br>' +
    'L11<br>L12<br>L13<br>L14<br>L15</div>';
flush();

const sc = document.getElementById('sc');
assert(sc.clientHeight === 100, 'the container clientHeight is its box: ' + sc.clientHeight);
assert(sc.scrollHeight > sc.clientHeight,
       'scrollHeight exceeds clientHeight when the content overflows: ' +
       sc.scrollHeight + ' vs ' + sc.clientHeight);

const maxScroll = sc.scrollHeight - sc.clientHeight;

let scrolls = 0;
sc.addEventListener('scroll', function () { scrolls++; });

sc.scrollTop = 1e6;
flush();
assert(Math.abs(sc.scrollTop - maxScroll) <= 1,
       'scrollTop clamps to the maximum: ' + sc.scrollTop + ' vs ' + maxScroll);
assert(scrolls === 1, 'a scroll that moved the offset fired one scroll event');

sc.scrollTop = sc.scrollTop;
flush();
assert(scrolls === 1, 'writing the offset it already has fires nothing');

sc.scrollTop = -50;
flush();
assert(sc.scrollTop === 0, 'a negative scrollTop clamps to 0: ' + sc.scrollTop);
assert(scrolls === 2, 'scrolling back to the top fired a second event');

// scrollTo / scrollBy, both argument shapes.
sc.scrollTo({ top: 10 });
flush();
assert(sc.scrollTop === 10, 'scrollTo({top}) moved the container: ' + sc.scrollTop);
sc.scrollTo(0, 20);
flush();
assert(sc.scrollTop === 20, 'scrollTo(x, y) moved the container: ' + sc.scrollTop);
sc.scrollBy(0, 5);
flush();
assert(sc.scrollTop === 25, 'scrollBy(x, y) is relative: ' + sc.scrollTop);
sc.scrollBy({ top: -5 });
flush();
assert(sc.scrollTop === 20, 'scrollBy({top}) is relative too: ' + sc.scrollTop);

// --- sticky bottom on a textContent append --------------------------------
// The classic transcript pattern: append and expect the view to follow. The
// append is not laid out yet when it happens, so this only works if the
// element remembers that it wants the end and the post-layout pass honours it.
root.innerHTML =
    '<div id="log" style="width:200px;height:100px;overflow-y:auto;' +
    'line-height:20px;">start</div>';
flush();
const log = document.getElementById('log');
let lines = '';
for (let i = 0; i < 40; i++) lines += 'line ' + i + '\n';
log.textContent = lines;
flush();
const logMax = log.scrollHeight - log.clientHeight;
assert(logMax > 0, 'the log overflows after the append: ' + log.scrollHeight);
assert(Math.abs(log.scrollTop - logMax) <= 1,
       'a textContent append into an overflow container sticks to the bottom: ' +
       log.scrollTop + ' vs ' + logMax);

root.innerHTML = '';
