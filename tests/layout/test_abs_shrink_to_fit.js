// An absolutely positioned box with width:auto shrinks to fit (CSS 2
// §10.3.7): min(max(min-content, available), max-content), where available is
// the containing block's width less its `left` and its own edges. Leaflet puts
// each popup at `left: <px>` inside a 0x0 pane, so there is no room at all and
// the popup is its min-content wide (the whole line, as it measures with
// white-space: nowrap); bro made it 0 px wide, and every popup a sliver.
document.body.innerHTML = `
<div style="position:absolute;left:0;top:0">
  <div id="p" style="position:absolute;left:300px;top:10px;font:16px sans-serif">
    <div id="c" style="padding:0 4px">alpha beta<br>gamma</div></div></div>
<div style="position:relative;width:400px;height:50px">
  <div id="q" style="position:absolute;left:300px;top:0;font:16px sans-serif">one two three four</div></div>
<span id="w1" style="font:16px sans-serif">alpha beta</span>
<span id="w2" style="font:16px sans-serif">alpha</span>
<span id="w3" style="font:16px sans-serif">one two three four</span>`;
const c = document.getElementById('c'), p = document.getElementById('p'), q = document.getElementById('q');
const w = (id) => document.getElementById(id).getBoundingClientRect().width;
c.style.whiteSpace = 'nowrap';
assert(Math.abs(c.offsetWidth - (w('w1') + 8)) <= 1, `nowrap: the longest line: ${c.offsetWidth} vs ${w('w1') + 8}`);
c.style.whiteSpace = '';
const minContent = Math.max(w('w2'), document.getElementById('w2').getBoundingClientRect().width);
assert(c.offsetWidth >= minContent && c.offsetWidth < w('w1') + 8, `no room: min-content, not 0: ${c.offsetWidth}`);
assert(p.offsetWidth === c.offsetWidth, 'the popup is its content wide');
// 100 px left of a 400 px block: the line wraps to fit there.
assert(q.offsetWidth <= 100 && q.offsetWidth > 0, `available = cb - left: ${q.offsetWidth}`);
assert(q.offsetHeight > 20, 'and wraps onto more lines');
console.log('test_abs_shrink_to_fit: OK');
