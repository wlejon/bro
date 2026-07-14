// Style invalidation: what a change to one element forces to re-resolve.
//
// The restyle is scoped — an inline-style write re-resolves the element itself
// and only reaches descendants through inherited values, while a selector input
// (class, id, attribute, :hover) re-matches the whole subtree. These are the
// cases that must keep working, and they are exactly what a too-narrow
// invalidation silently breaks: the DOM says one thing, the screen shows the
// style from three frames ago.

const root = document.getElementById('root');

const sheet = document.createElement('style');
sheet.textContent = `
  #inv-btn { color: rgb(1,1,1); }
  .dark #inv-btn { color: rgb(2,2,2); }
  #inv-app:hover #inv-btn { color: rgb(3,3,3); }
  .wide #inv-btn { width: 300px; }
  #inv-var { color: var(--inv-accent, rgb(4,4,4)); }
  #inv-app.subjectonly { color: rgb(9,9,9); }
`;
document.head.appendChild(sheet);

root.innerHTML = `
  <div id="inv-app">
    <div>
      <div><span id="inv-btn" style="display:inline-block">B</span></div>
      <div id="inv-var">V</div>
      <div id="inv-inherit">I</div>
    </div>
  </div>`;
flush();

const app = document.getElementById('inv-app');
const btn = document.getElementById('inv-btn');
const color = (id) =>
    getComputedStyle(document.getElementById(id)).getPropertyValue('color').replace(/\s+/g, '');

assert(color('inv-btn') === 'rgb(1,1,1)', 'baseline color: ' + color('inv-btn'));

// A class on an ancestor re-matches a descendant rule two levels down.
app.className = 'dark';
flush();
assert(color('inv-btn') === 'rgb(2,2,2)', '.dark #inv-btn: ' + color('inv-btn'));

app.className = '';
flush();
assert(color('inv-btn') === 'rgb(1,1,1)', 'class removed: ' + color('inv-btn'));

// A class no selector uses in an ancestor position re-matches only the element
// it is on — but it must still re-match THAT element. This is the fast path
// (`row.classList.toggle('on')`), so it is the one that would silently stop
// updating if the scoping were wrong.
app.className = 'subjectonly';
flush();
assert(color('inv-app') === 'rgb(9,9,9)', 'subject-only class: ' + color('inv-app'));
app.className = '';
flush();
assert(color('inv-app') !== 'rgb(9,9,9)', 'subject-only class removed');

// An inline write to an inherited property reaches descendants.
app.style.setProperty('color', 'rgb(5,5,5)');
flush();
assert(color('inv-inherit') === 'rgb(5,5,5)', 'inherited color: ' + color('inv-inherit'));

app.style.removeProperty('color');
flush();
assert(color('inv-inherit') !== 'rgb(5,5,5)', 'inherited color removed');

// Custom properties inherit, so a var() user downstream must re-resolve.
app.style.setProperty('--inv-accent', 'rgb(6,6,6)');
flush();
assert(color('inv-var') === 'rgb(6,6,6)', 'var(--inv-accent): ' + color('inv-var'));

// A paint-only inline write inherits nothing and must leave descendants alone.
app.style.setProperty('opacity', '0.5');
flush();
assert(color('inv-btn') === 'rgb(1,1,1)', 'descendant intact after opacity');

// A class change that moves a descendant's geometry must relayout it, not just
// repaint it — the layout promotion has to survive the scoped restyle.
const w0 = btn.getBoundingClientRect().width;
app.className = 'wide';
flush();
assert(btn.getBoundingClientRect().width === 300 && w0 !== 300,
       '.wide #inv-btn width: ' + btn.getBoundingClientRect().width);
app.className = '';
flush();

// :hover is a selector input too: hovering the ancestor re-matches the subtree.
const r = app.getBoundingClientRect();
mouseMove(r.left + 2, r.top + 2);
flush();
assert(color('inv-btn') === 'rgb(3,3,3)', 'hover ancestor: ' + color('inv-btn'));

mouseMove(r.left + 2, r.top + r.height + 200);
flush();
assert(color('inv-btn') === 'rgb(1,1,1)', 'unhover: ' + color('inv-btn'));

// A stylesheet added at runtime changes what matches, everywhere.
const late = document.createElement('style');
late.textContent = '#inv-btn { color: rgb(7,7,7); }';
document.head.appendChild(late);
flush();
assert(color('inv-btn') === 'rgb(7,7,7)', 'runtime sheet: ' + color('inv-btn'));

// `inherit` on a property that does NOT normally inherit ties a descendant to a
// parent value the inherited-value diff never looks at. Setting it on the parent
// must still reach the child.
const forced = document.createElement('style');
forced.textContent = '#inv-forced { background-color: inherit; }';
document.head.appendChild(forced);
root.innerHTML += '<div id="inv-app2"><div id="inv-forced">F</div></div>';
flush();

const app2 = document.getElementById('inv-app2');
const bg = () => getComputedStyle(document.getElementById('inv-forced'))
                   .getPropertyValue('background-color').replace(/\s+/g, '');
app2.style.setProperty('background-color', 'rgb(8,8,8)');
flush();
assert(bg() === 'rgb(8,8,8)', 'background-color:inherit follows parent: ' + bg());

// ── :hover is scoped by what a hover rule can actually name ────────────────
// The pointer moving re-resolves the elements whose :hover flipped, and then
// only the elements some rule pairs with a :hover ancestor or sibling. Each
// shape below is a way that scoping can be wrong: too narrow and the style
// silently never updates, too wide and every mouse move restyles whatever
// container the pointer happens to be inside.
const hs = document.createElement('style');
hs.textContent = `
  #hv-list { padding: 30px; }
  #hv-list .row:hover { color: rgb(13,13,13); }
  #hv-list .row:hover .label { color: rgb(10,10,10); }
  #hv-list .row:hover b { color: rgb(11,11,11); }
  #hv-tabs .tab:hover + .panel { color: rgb(12,12,12); }
`;
document.head.appendChild(hs);

let fill = '';
for (let i = 0; i < 100; i++) fill += '<span class="label">f</span>';
root.innerHTML = `
  <div id="hv-list">
    <div class="row" id="hv-r1"><span class="label" id="hv-l1">a</span><b id="hv-b1">B</b></div>
    <div class="row" id="hv-r2"><span class="label" id="hv-l2">c</span></div>
    <div id="hv-fill">${fill}</div>
  </div>
  <div id="hv-tabs">
    <div class="tab" id="hv-t1">t</div>
    <div class="panel" id="hv-p1">p</div>
  </div>`;
flush();

const list = document.getElementById('hv-list');
const away = () => { mouseMove(2, 2); flush(); };
const hover = (id) => {
  const b = document.getElementById(id).getBoundingClientRect();
  mouseMove(b.left + b.width / 2, b.top + b.height / 2);
  flush();
};
away();

// Descendant of the hovered element, keyed by class and by tag.
hover('hv-r1');
assert(color('hv-r1') === 'rgb(13,13,13)', 'hovered row itself: ' + color('hv-r1'));
assert(color('hv-l1') === 'rgb(10,10,10)', '.row:hover .label: ' + color('hv-l1'));
assert(color('hv-b1') === 'rgb(11,11,11)', '.row:hover b: ' + color('hv-b1'));
// The other row is untouched — its :hover never flipped.
assert(color('hv-l2') !== 'rgb(10,10,10)', 'sibling row label unaffected');

away();
assert(color('hv-l1') !== 'rgb(10,10,10)', 'label reverts on unhover: ' + color('hv-l1'));

// Sibling: the subject is OUTSIDE the hovered element's subtree, so the scope
// has to reach up to the parent to find it.
hover('hv-t1');
assert(color('hv-p1') === 'rgb(12,12,12)', '.tab:hover + .panel: ' + color('hv-p1'));
away();
assert(color('hv-p1') !== 'rgb(12,12,12)', 'panel reverts on unhover: ' + color('hv-p1'));

// And the scope: hovering the list's padding flips :hover on #hv-list, which no
// rule names on the :hover side — so none of the 100 labels under it re-resolve.
// (Hovering it used to re-match its whole subtree, which is what made dragging
// across a big panel cost a full-document restyle.)
const lb = list.getBoundingClientRect();
away();
perf.reset();
mouseMove(lb.left + 4, lb.top + 4);
flush();
const styled = perf.stats().elementsStyled;
assert(styled < 12, 'hovering a container no hover rule names stays O(chain): ' +
                    styled + ' elements re-resolved');

root.innerHTML = '';
