// The focus change of a press is the mousedown's default action: mousedown
// is dispatched first, and when the page cancels it the focus stays where
// it was (a suggestion list under a text field cancels it, so a click on a
// suggestion leaves the field focused and its list open for the click).
// The focus used to move, blurring the field, before the page saw the press.
// Uncancelled, the order is the web's: mousedown, change and blur on the
// field left, focus on the one pressed.

const root = document.getElementById('root');
root.innerHTML = '<input type="text" id="f" value="mal">' +
  '<div id="list" style="width:200px;height:40px;background:#888">suggestion</div>' +
  '<input type="text" id="g" value="">';
const f = document.getElementById('f');
const g = document.getElementById('g');
const list = document.getElementById('list');
flush();

const at = (el) => { const r = el.getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; };
const log = [];
for (const [el, name] of [[f, 'f'], [g, 'g'], [list, 'list']]) {
  for (const t of ['mousedown', 'blur', 'focus', 'change', 'click']) el.addEventListener(t, () => log.push(`${name}:${t}`));
}

f.focus();
flush();
assert(document.activeElement === f, 'the field has the focus');

// Cancelled: the click on the list happens with the field still focused.
const cancel = (e) => e.preventDefault();
list.addEventListener('mousedown', cancel);
log.length = 0;
click(...at(list), 0);
flush();
assert(document.activeElement === f, 'a cancelled mousedown leaves the focus in the field, got ' + (document.activeElement && document.activeElement.id));
assert(log.join() === 'list:mousedown,list:click', 'no blur, and the click lands: ' + log.join());
list.removeEventListener('mousedown', cancel);

// Cancelled on another field: it is not focused either.
g.addEventListener('mousedown', cancel);
log.length = 0;
click(...at(g), 0);
flush();
assert(document.activeElement === f, 'a field whose mousedown is cancelled does not take the focus');
g.removeEventListener('mousedown', cancel);

// Not cancelled: mousedown first, then the field left reports its change and blurs, then focus.
f.value = 'mallard';
textInput('s');
flush();
log.length = 0;
click(...at(g), 0);
flush();
assert(document.activeElement === g, 'an uncancelled press moves the focus');
assert(log.join() === 'g:mousedown,f:change,f:blur,g:focus,g:click', 'the web\'s order: ' + log.join());
// And the mousedown saw the focus where it was.
let seen = null;
const peek = () => { seen = document.activeElement && document.activeElement.id; };
f.addEventListener('mousedown', peek);
click(...at(f), 0);
flush();
assert(seen === 'g', 'mousedown runs before the focus moves, saw ' + seen);
assert(document.activeElement === f, 'and then it moves');
