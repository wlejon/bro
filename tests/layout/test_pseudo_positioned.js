// An absolutely positioned ::before / ::after is laid out and painted with the
// width and height it asks for. Two things had to hold: the positioned pass
// visits the pseudo boxes (they hang off the element outside its children),
// and a positioned pseudo is blockified like any positioned box, since inline
// layout ignores width and height. And var() in a pseudo's declarations has
// to be substituted, or a token colour reaches paint unparsed and draws
// nothing. Kit list headers draw their column
// dividers this way (an empty ::after rule inside a grip), as do badge dots.

const root = document.getElementById('root') || document.body;
const style = document.createElement('style');
document.head.appendChild(style);
style.textContent = `
  body { margin: 0; background: rgb(0, 0, 0); }
  .host { position: relative; width: 200px; height: 40px; margin: 10px; background: rgb(40, 40, 40); }
  .empty::after { content: ''; position: absolute; left: 10px; top: 5px; bottom: 5px; width: 4px; background: rgb(255, 0, 0); }
  .text::before { content: 'x'; position: absolute; left: 10px; top: 5px; height: 30px; width: 30px; background: rgb(0, 255, 0); color: rgb(0, 255, 0); }
  .grip { position: absolute; top: 0; bottom: 0; right: -12px; width: 12px; }
  :root { --rule: rgb(255, 255, 0); }
  .grip::after { content: ''; position: absolute; left: 5px; top: 9px; bottom: 9px; width: 1px; background: var(--rule); }
  .flexed { display: flex; }
  .flexed::after { content: ''; width: 6px; height: 20px; background: rgb(0, 0, 255); }
`;
root.innerHTML =
  '<div class="host empty" id="e"></div>' +
  '<div class="host text" id="t"></div>' +
  '<div class="host" id="g"><div class="grip"></div></div>' +
  '<div class="host flexed" id="f"></div>';
flush();
advanceTime(16);

const at = (id, dx, dy) => {
  const r = document.getElementById(id).getBoundingClientRect();
  return getPixel(r.left + dx, r.top + dy);
};
const is = (p, r, g, b) => Math.abs(p.r - r) < 30 && Math.abs(p.g - g) < 30 && Math.abs(p.b - b) < 30;
const show = (p) => `rgb(${p.r}, ${p.g}, ${p.b})`;

let p = at('e', 12, 20);
assert(is(p, 255, 0, 0), 'an empty positioned ::after paints its 4px width: ' + show(p));
p = at('e', 20, 20);
assert(is(p, 40, 40, 40), 'and no wider: ' + show(p));

p = at('t', 35, 20);
assert(is(p, 0, 255, 0), 'a positioned ::before with text takes its own width, not its text width: ' + show(p));

p = at('g', 200 + 5, 20);
assert(is(p, 255, 255, 0), 'a rule inside a positioned grip, outside its host, paints its var() colour: ' + show(p));

p = at('f', 3, 10);
assert(is(p, 0, 0, 255), 'an ::after in a flex container is a flex item with its own size: ' + show(p));

console.log('test_pseudo_positioned PASSED');
