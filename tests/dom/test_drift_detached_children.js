// Clearing a container must DETACH its element children, not destroy them.
//
// jQuery's buildFragment clears a temporary container with `textContent = ''`
// and then reparents the children it just parsed; every "append this HTML"
// path in that library goes through it. If clearing frees the element nodes,
// the wrapper a program is still holding points at freed memory — in this
// stack that shows up as an inert wrapper whose tagName is undefined and whose
// re-insertion does nothing.
//
// Text and comment children ARE destroyed: nothing can hold one after the
// clear except through a wrapper the same clear invalidates, and keeping them
// would be an unbounded leak.

const root = document.getElementById('root');

const holder = document.createElement('div');
root.appendChild(holder);
holder.innerHTML = '<span id="kept-a">A</span><span id="kept-b">B</span>';

const a = holder.children[0];
const b = holder.children[1];
assert(a.tagName === 'SPAN', 'child A is a span before the clear');

// --- textContent = '' detaches ------------------------------------------
holder.textContent = '';
assert(holder.children.length === 0, 'textContent = "" emptied the container');
assert(a.tagName === 'SPAN', 'the detached child A still answers tagName');
assert(b.textContent === 'B', 'the detached child B still answers textContent');
assert(a.parentNode === null, 'a detached child has no parent');
assert(document.getElementById('kept-a') === null,
       'a detached child is unregistered from getElementById');

// Re-insertion works: this is the whole reason the child survived.
holder.appendChild(a);
holder.appendChild(b);
assert(holder.children.length === 2, 'both detached children re-inserted');
assert(holder.textContent === 'AB', 're-inserted children carry their text');
assert(document.getElementById('kept-a') === a,
       're-inserting re-registers the id');

// --- innerHTML replacement detaches too ----------------------------------
const c = holder.children[0];
holder.innerHTML = '<i>fresh</i>';
assert(holder.children.length === 1, 'innerHTML replaced the children');
assert(c.tagName === 'SPAN', 'innerHTML left the old child usable');
c.textContent = 'still writable';
assert(c.textContent === 'still writable', 'the old child still takes writes');

// --- setting textContent to real text also detaches -----------------------
holder.innerHTML = '<b id="kept-c">C</b>';
const d = holder.children[0];
holder.textContent = 'plain';
assert(holder.textContent === 'plain', 'textContent replaced the subtree');
assert(d.tagName === 'B', 'the replaced element child is still usable');
assert(document.getElementById('kept-c') === null,
       'the replaced element child left the id map');

root.innerHTML = '';
