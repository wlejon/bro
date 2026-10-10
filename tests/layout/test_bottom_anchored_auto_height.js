// A position: fixed (or absolute) box anchored by `bottom` alone, with no top
// and no height, is as tall as its content, its bottom edge `bottom` above the
// containing block's. A column-reverse flex container — a toast stack whose
// newest toast sits nearest the anchor — came out zero tall: the reversed
// line's extent was read off a cursor that finishes at its start.

const root = document.getElementById('root');
const near = (a, b) => Math.abs(a - b) < 0.5;
const vh = window.innerHeight;

for (const pos of ['fixed', 'absolute']) {
  for (const dir of ['column-reverse', 'column', 'row']) {
    root.innerHTML =
      '<div id="cb" style="position:relative;height:' + vh + 'px">' +
      '<div id="stack" style="position:' + pos + ';bottom:40px;left:50%;transform:translateX(-50%);' +
      'width:380px;display:flex;flex-direction:' + dir + ';gap:8px">' +
      '<div id="a" style="height:30px;flex:none">newest</div><div id="b" style="height:20px;flex:none">older</div>' +
      '</div></div>';
    const s = document.getElementById('stack').getBoundingClientRect();
    const a = document.getElementById('a').getBoundingClientRect();
    const b = document.getElementById('b').getBoundingClientRect();
    const what = pos + ' ' + dir;
    const want = dir === 'row' ? 30 : 58;
    assert(near(s.height, want), what + ': height is the content\'s ' + want + ', got ' + s.height);
    // fixed: the viewport is the containing block; absolute: the relative div.
    const cbBottom = pos === 'fixed' ? vh : document.getElementById('cb').getBoundingClientRect().bottom;
    assert(near(s.bottom, cbBottom - 40),
           what + ': bottom edge 40px above the containing block\'s ' + cbBottom + ', got ' + s.bottom);
    if (dir === 'column-reverse') {
      assert(near(a.bottom, s.bottom) && near(b.bottom, a.top - 8),
             what + ': the first item sits at the bottom, the next above it (a ' + a.top + '..' +
             a.bottom + ', b ' + b.top + '..' + b.bottom + ')');
    }
  }
}

// Toasts appended after the stack is laid out (empty, then one, then two).
root.innerHTML = '<div id="t" style="position:fixed;bottom:60px;left:0;width:300px;display:flex;' +
                 'flex-direction:column-reverse;gap:8px"></div>';
const stack = document.getElementById('t');
flush();
assert(near(stack.getBoundingClientRect().height, 0), 'an empty stack is zero tall');
for (let i = 1; i <= 2; i++) {
  const n = document.createElement('div');
  n.style.cssText = 'height:40px;padding:4px;border:1px solid';
  n.textContent = 'toast ' + i;
  stack.appendChild(n);
  flush();
  const r = stack.getBoundingClientRect();
  const want = i * 40 + (i - 1) * 8;
  assert(near(r.height, want), i + ' toast(s): stack is ' + want + ' tall, got ' + r.height);
  assert(near(r.bottom, vh - 60), i + ' toast(s): anchored 60px up, bottom ' + r.bottom);
}

root.innerHTML = '';
