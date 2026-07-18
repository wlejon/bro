// innerHTML parses in the context of the element receiving it.
//
// The HTML fragment parsing algorithm seeds the tree builder with a context
// element. Without one the builder sits in "in body" insertion mode, where
// <tr>, <td>, <tbody>, <thead>, <tfoot> and <caption> start tags are parse
// errors and get dropped, keeping only their text — so table markup assigned
// through innerHTML silently vanished while its text survived.

const root = document.getElementById('root');
const tags = el => Array.from(el.children).map(c => c.tagName.toLowerCase()).join(',');

// --- table internals, the case that used to collapse ---

root.innerHTML = '<table><tbody id="tb"><tr><td>A</td><td>B</td></tr></tbody></table>';
flush();
const tb = document.getElementById('tb');
assert(tb !== null, 'tbody parses inside a full table');

tb.innerHTML = '<tr><td>1</td><td>2</td></tr><tr><td>3</td><td>4</td></tr>';
flush();
assert(tb.children.length === 2, 'tbody.innerHTML creates two rows');
assert(tags(tb) === 'tr,tr', 'tbody children are <tr>, not bare text');
assert(tb.children[0].children.length === 2, 'first row keeps both cells');

tb.children[0].innerHTML = '<td>X</td><td>Y</td><td>Z</td>';
flush();
assert(tags(tb.children[0]) === 'td,td,td', 'tr.innerHTML creates cells');

const table = root.children[0];
table.innerHTML = '<tbody><tr><td>q</td></tr></tbody>';
flush();
assert(tags(table) === 'tbody', 'table.innerHTML accepts a tbody');

// Parsed is not the same as laid out: a row with cells must occupy height.
table.innerHTML = '<tbody><tr><td>a</td></tr></tbody>';
flush();
const oneRow = table.getBoundingClientRect().height;
table.innerHTML = '<tbody><tr><td>a</td></tr><tr><td>b</td></tr><tr><td>c</td></tr></tbody>';
flush();
const threeRows = table.getBoundingClientRect().height;
assert(oneRow > 0, 'a single-row table has height');
assert(threeRows > oneRow, 'three rows are taller than one');

// --- contexts that already worked must keep working ---

root.innerHTML = '<ul id="u"></ul><select id="s"></select><dl id="d"></dl>' +
                 '<fieldset id="f"></fieldset><span id="sp"></span>';
flush();
document.getElementById('u').innerHTML = '<li>a</li><li>b</li>';
document.getElementById('s').innerHTML = '<option>a</option><option>b</option>';
document.getElementById('d').innerHTML = '<dt>t</dt><dd>d</dd>';
document.getElementById('f').innerHTML = '<legend>L</legend><input>';
document.getElementById('sp').innerHTML = '<b>bold</b>';
flush();
assert(tags(document.getElementById('u')) === 'li,li', 'ul > li');
assert(tags(document.getElementById('s')) === 'option,option', 'select > option');
assert(document.getElementById('s').value === 'a', 'select value reads back');
assert(tags(document.getElementById('d')) === 'dt,dd', 'dl > dt,dd');
assert(tags(document.getElementById('f')) === 'legend,input', 'fieldset > legend,input');
assert(tags(document.getElementById('sp')) === 'b', 'span > b');

// A <table> start tag in the fragment establishes table mode by itself, so
// this path never depended on the context element.
root.innerHTML = '<table><tbody><tr><td>z</td></tr></tbody></table>';
flush();
assert(root.querySelectorAll('td').length === 1, 'full table round-trips under a div');

// --- RCDATA context: textarea content stays text, per spec ---

root.innerHTML = '<textarea id="ta"></textarea>';
flush();
const ta = document.getElementById('ta');
ta.innerHTML = '<b>x</b>';
flush();
assert(ta.children.length === 0, 'textarea does not parse markup into elements');
assert(ta.textContent.trim() === '<b>x</b>', 'textarea keeps the markup as text');

// --- unknown tags fall back to the historical wrapper path ---

root.innerHTML = '<my-widget id="cw"></my-widget>';
flush();
const cw = document.getElementById('cw');
cw.innerHTML = '<div>c</div>';
flush();
assert(tags(cw) === 'div', 'custom element still parses its children');

console.log('innerHTML fragment context: all checks passed');
