const root = document.getElementById('root');
root.innerHTML = '';
const stage = document.createElement('div');
stage.style.cssText = 'position:relative;width:600px;height:400px;background:#101010;overflow:hidden;';
const host = document.createElement('div');
host.style.cssText = 'position:absolute;inset:0;overflow:hidden;';
stage.appendChild(host);
root.appendChild(stage);

function label(text) {
  const el = document.createElement('div');
  el.style.cssText = 'position:absolute;transform:translate(-50%,-50%);padding:10px 16px;background:#20ff40;border:1px solid #fff;font:12px monospace;color:#20ff40;white-space:nowrap;';
  el.textContent = text;
  el.hidden = true;
  host.appendChild(el);
  return el;
}
function place(el, x, y) {
  el.style.left = `${x}px`;
  el.style.top = `${y}px`;
  el.hidden = false;
}
function green(x, y) {
  const p = getPixel(x, y + 12);
  return p.g > 150 && p.r < 100;
}

const a = label('ONE');
const b = label('TWO');
const c = label('THREE');
flush();
assert(getComputedStyle(a).display === 'none', 'a fresh hidden element computes display none');
assert(a.getBoundingClientRect().width === 0, 'a hidden element has no box');

place(a, 100, 100);
place(b, 300, 200);
place(c, 450, 300);
flush();
assert(getComputedStyle(b).display === 'block', 'clearing hidden restores the display');
assert(green(100, 100), 'label a paints once shown');
assert(green(300, 200), 'label b paints once shown');
assert(green(450, 300), 'label c paints once shown');

place(a, 500, 80);
b.hidden = true;
c.setAttribute('hidden', '');
flush();
assert(getComputedStyle(b).display === 'none', 'hidden = true computes display none');
assert(b.getBoundingClientRect().width === 0, 'hidden = true drops the box');
assert(!green(100, 100), 'a moved label is erased from its old spot');
assert(green(500, 80), 'a moved label paints at its new spot');
assert(!green(300, 200), 'hidden = true erases the label the same frame');
assert(!green(450, 300), 'setAttribute hidden erases the label the same frame');

b.removeAttribute('hidden');
flush();
assert(green(300, 200), 'removeAttribute hidden paints the label again where it was');

const style = document.createElement('style');
style.textContent = '.keep[hidden] { display: block; }';
document.head.appendChild(style);
c.className = 'keep';
flush();
assert(getComputedStyle(c).display === 'block', 'an author rule on [hidden] outranks the UA rule');
assert(green(450, 300), 'an author rule on [hidden] keeps the label painted');
