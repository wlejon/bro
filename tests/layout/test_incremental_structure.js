// Structural invalidation: what a DOM mutation forces layout to redo.
//
// A change to one element's child list rebuilds that element's layout children
// and leaves every other subtree's cached geometry alone. Two things have to
// hold, and they pull against each other:
//
//   - correctness: the mutated subtree really does re-lay out, and so does
//     anything whose geometry depends on it.
//   - scope: nothing else does. This is the half that fails silently — the page
//     still looks right, it just costs O(document) per keystroke — so it is
//     asserted here with perf.stats().nodesLaidOut rather than left to a
//     benchmark nobody runs.

const root = document.getElementById('root');

const sheet = document.createElement('style');
sheet.textContent = `
  #st-app { width: 600px; font: 16px Arial; }
  #st-list div { height: 20px; }
  #st-other div { height: 20px; }
  #st-hidden { display: none; }
`;
document.head.appendChild(sheet);

root.innerHTML = `
  <div id="st-app">
    <div id="st-list"><div>a</div></div>
    <div id="st-other"><div>x</div><div>y</div><div>z</div></div>
    <div id="st-hidden">boo</div>
    <select id="st-sel"><option>one</option></select>
  </div>`;
flush();

const list = document.getElementById('st-list');
const other = document.getElementById('st-other');
const h = (id) => document.getElementById(id).getBoundingClientRect().height;

// ── correctness ─────────────────────────────────────────────────────────────

assert(h('st-list') === 20, 'baseline list height: ' + h('st-list'));
const otherH0 = h('st-other');
const otherY0 = other.getBoundingClientRect().top;

// appendChild grows the container it was appended to.
const row = document.createElement('div');
row.textContent = 'b';
list.appendChild(row);
flush();
assert(h('st-list') === 40, 'list grew on appendChild: ' + h('st-list'));

// ...and pushes the following sibling down, without re-laying it out from
// scratch: its own height is unchanged, only its position moved.
assert(h('st-other') === otherH0, 'sibling height unchanged: ' + h('st-other'));
assert(other.getBoundingClientRect().top === otherY0 + 20,
       'sibling pushed down by 20: ' + (other.getBoundingClientRect().top - otherY0));

// innerHTML replaces children — the single most common app-side DOM update.
list.innerHTML = '<div>p</div><div>q</div><div>r</div>';
flush();
assert(h('st-list') === 60, 'list rebuilt via innerHTML: ' + h('st-list'));

list.innerHTML = '';
flush();
assert(h('st-list') === 0, 'list emptied via innerHTML: ' + h('st-list'));

// removeChild shrinks it back.
list.innerHTML = '<div>p</div><div>q</div>';
flush();
list.removeChild(list.lastChild);
flush();
assert(h('st-list') === 20, 'list shrank on removeChild: ' + h('st-list'));

// A text child is layout too — it gets a layout node like an element does — so
// appending one must relayout. (An empty div has no height; a div with a line of
// text does.) Appended to #st-app, not #st-list, whose rule pins the height.
const bare = document.createElement('div');
document.getElementById('st-app').appendChild(bare);
flush();
assert(bare.getBoundingClientRect().height === 0, 'empty div has no height');
bare.appendChild(document.createTextNode('text'));
flush();
assert(bare.getBoundingClientRect().height > 0,
       'text node append relaid out: ' + bare.getBoundingClientRect().height);

// display:none -> block is a *style* change, not a structural one: the layout
// tree holds a node for a display:none element and layout zero-sizes it. If the
// tree's shape depended on display, this would come back 0.
const hidden = document.getElementById('st-hidden');
assert(h('st-hidden') === 0, 'hidden starts collapsed');
hidden.style.display = 'block';
flush();
assert(h('st-hidden') > 0, 'display:none -> block laid out: ' + h('st-hidden'));
hidden.style.display = 'none';
flush();
assert(h('st-hidden') === 0, 'display:block -> none collapsed again');

// <select> owns its <option> children — they get no layout node of their own, so
// a structural mark on the option has nowhere to land and is redirected to the
// select. Adding one must still re-measure the control.
const sel = document.getElementById('st-sel');
const selW0 = sel.getBoundingClientRect().width;
const opt = document.createElement('option');
opt.textContent = 'a considerably longer option label';
sel.appendChild(opt);
flush();
assert(sel.getBoundingClientRect().width > selW0,
       'select re-measured after option append: ' +
       sel.getBoundingClientRect().width + ' vs ' + selW0);

// ── scope ───────────────────────────────────────────────────────────────────
//
// Now the half that fails quietly. Fill #st-other with enough nodes that a
// document-wide relayout is unmistakable in the counts, then mutate #st-list and
// check the pass didn't touch them.

let html = '';
for (let i = 0; i < 200; i++) html += '<div>row ' + i + '</div>';
other.innerHTML = html;
flush();

const bulk = document.querySelectorAll('#st-other div').length;
assert(bulk === 200, 'bulk rows built: ' + bulk);

perf.reset();
const tag = document.createElement('div');
tag.textContent = 'one more';
list.appendChild(tag);
flush();
const laid = perf.stats().nodesLaidOut;

// The mutated container, its new child, and the ancestor chain up to the root
// have to lay out. The 200 untouched rows must not. The exact number depends on
// how many nodes sit between here and the root, so this asserts the order of
// magnitude, which is the thing that actually regresses: a document-wide
// invalidation would put this well north of 200.
assert(laid > 0, 'something laid out');
assert(laid < 60,
       'appendChild stayed local: laid out ' + laid + ' nodes with 200 untouched ' +
       'siblings present (a document-wide relayout would be 200+)');

// And the untouched siblings kept their geometry.
const rowsAfter = document.querySelectorAll('#st-other div');
assert(rowsAfter[0].getBoundingClientRect().height === 20, 'untouched row intact');
assert(rowsAfter[199].getBoundingClientRect().height === 20, 'last untouched row intact');

root.innerHTML = '';
