// `button { font: inherit }` makes a button use its parent's font: family,
// size, weight, style and line-height. The `font` shorthand with a CSS-wide
// keyword sets every longhand to that keyword; it used to expand to a
// font-size of "inherit" over the shorthand's default family (sans-serif), so
// buttons stayed in a default font. The UA's own form-control font must not
// override the author's inherit either.

const root = document.getElementById('root');
root.innerHTML =
  '<style>.p { font: italic 600 20px/30px Georgia, serif; }' +
  '.p button, .p input, .p select, .p textarea { font: inherit; }</style>' +
  '<div class="p" id="p"><span id="ref">Save changes</span>' +
  '<button id="b">Save changes</button><input id="i" value="x"><select id="s"><option>x</option></select>' +
  '<textarea id="t">x</textarea></div>' +
  '<button id="plain">Save changes</button>';

const ps = getComputedStyle(document.getElementById('p'));
for (const id of ['b', 'i', 's', 't']) {
  const cs = getComputedStyle(document.getElementById(id));
  for (const prop of ['fontFamily', 'fontSize', 'fontWeight', 'fontStyle', 'lineHeight']) {
    assert(cs[prop] === ps[prop],
           '#' + id + ' inherits ' + prop + ': parent ' + ps[prop] + ', got ' + cs[prop]);
  }
}

// And the text is measured in that font: the button's label is as wide as the
// same words in a span beside it (plus the button's own padding and border).
const ref = document.getElementById('ref').getBoundingClientRect().width;
const b = document.getElementById('b');
const bcs = getComputedStyle(b);
const edges = ['paddingLeft', 'paddingRight', 'borderLeftWidth', 'borderRightWidth']
  .reduce((s, k) => s + parseFloat(bcs[k]), 0);
const label = b.getBoundingClientRect().width - edges;
assert(Math.abs(label - ref) < 1,
       'the button label is measured in the inherited font: ' + label.toFixed(2) +
       ' vs span ' + ref.toFixed(2));

// A button without the rule keeps the UA font (a different size here).
const plain = getComputedStyle(document.getElementById('plain'));
assert(plain.fontSize !== '20px', 'a button without font: inherit keeps the UA size, got ' + plain.fontSize);

root.innerHTML = '';
