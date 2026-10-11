// The kinds of box the paint cache (layout/paint_cache.h) once never reused,
// and the stamps that now keep reusing them honest, on a page with nothing
// animating (where a stale span stays on screen): inline <svg> (its paint is
// its own subtree's: an attribute or a child changing stamps it), form
// controls (their state reaches them through attributes and restyles; one
// with focus paints a caret, a selection and typed text that move with no
// restyle, and is recorded every pass) and numbered list items (a marker
// counts its siblings: its span checks its parent's stamp too). After each
// change the frame the cache made is compared with the same frame recorded
// and rastered whole (setPaintCache(false)), on both alternating pools.

document.body.style.margin = '0';
document.body.style.background = '#fff';

function diffCount(a, b) {
    if (!a || !b || a.width !== b.width || a.height !== b.height) return -1;
    let n = 0;
    for (let i = 0; i < a.data.length; i += 4)
        if (Math.abs(a.data[i] - b.data[i]) > 16 || Math.abs(a.data[i + 1] - b.data[i + 1]) > 16 ||
            Math.abs(a.data[i + 2] - b.data[i + 2]) > 16)
            ++n;
    return n;
}
function check(what) {
    flush();
    const cached = [frameImage(), frameImage()];
    setPaintCache(false);
    const full = frameImage();
    setPaintCache(true);
    for (let k = 0; k < 2; ++k) {
        const n = diffCount(cached[k], full);
        assert(n === 0, `${what}: capture ${k + 1} has ${n} pixels apart from a full re-record`);
    }
}

// Each kind in a box of its own, so its span is not recorded again for
// anything else on the page.
function box() {
    const d = document.createElement('div');
    d.style.cssText = 'margin: 8px; padding: 4px; border: 1px solid #888';
    document.body.appendChild(d);
    return d;
}
const svgBox = box();
svgBox.innerHTML = '<svg width="80" height="40"><circle id="c" cx="20" cy="20" r="10" fill="#c33"/>' +
                   '<g id="g"><rect x="40" y="10" width="20" height="20" fill="#36c"/></g></svg>';
const listBox = box();
listBox.innerHTML = '<ol id="ol"><li><b>one</b></li><li><b>two</b></li><li><b>three</b></li></ol>';
const fieldBox = box();
fieldBox.innerHTML = '<input id="f" value="hello world" style="width: 200px; font: 16px sans-serif">' +
                     '<input id="cb" type="checkbox"><select id="s"><option>alpha</option><option>beta</option></select>';
flush();
setPaintCache(true);
frameImage();
frameImage();
const p0 = perf.stats().paint;

const c = document.getElementById('c');
c.setAttribute('fill', '#3a3');
check('svg fill');
c.setAttribute('r', '6');
check('svg r');
document.getElementById('g').appendChild(document.createElementNS('http://www.w3.org/2000/svg', 'circle'));
document.querySelector('#g circle').setAttribute('cx', '70');
document.querySelector('#g circle').setAttribute('cy', '20');
document.querySelector('#g circle').setAttribute('r', '5');
check('svg child added');

const ol = document.getElementById('ol');
const li = document.createElement('li');
li.innerHTML = '<b>zero</b>';
ol.insertBefore(li, ol.firstChild);
check('list item put in first: every number after it moves');
ol.children[1].setAttribute('value', '7');
check('list item value: the ones after it renumber');
ol.firstChild.remove();
check('list item taken out');
ol.setAttribute('start', '4');
check('list start');

document.getElementById('cb').checked = true;
check('checkbox checked');
document.getElementById('s').selectedIndex = 1;
check('select index');
const f = document.getElementById('f');
f.value = 'paint cache';
check('field value');
f.focus();
check('field focused');
f.setSelectionRange(2, 8);
check('field selection');
textInput('Q');
check('field typed');
f.setSelectionRange(1, 1);
check('field caret moved');
f.blur();
check('field blurred');

// Unchanged, they are reused: an idle frame records nothing.
const p1 = perf.stats().paint;
frameImage();
const p2 = perf.stats().paint;
assert(p2.recorded === p1.recorded, `an idle frame records no span (${p2.recorded - p1.recorded})`);
assert(p1.reused > p0.reused, 'spans were reused across the changes');
console.log('paint cache kinds: checked');
