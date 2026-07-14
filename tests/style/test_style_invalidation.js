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

root.innerHTML = '';
