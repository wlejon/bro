// element.click() default action on checkbox/radio inputs: toggle checked
// state and fire change + input, mirroring the hit-tested mouse path in
// replaced_elements.cpp. Pre-fix the synthetic click only dispatched the
// click event — a scripted $('box').click() left the checkbox unchanged and
// never fired change, so app state keyed off the change listener silently
// desynced (krea2-lab's live toggle in headless tests).

const root = document.getElementById('root');
assert(root !== null, 'root element exists');

// ── checkbox: click toggles + fires change and input ────────────────────────
const box = document.createElement('input');
box.setAttribute('type', 'checkbox');
box.id = 'box';
root.appendChild(box);
flush();

let changes = 0, inputs = 0;
box.addEventListener('change', () => { changes++; });
box.addEventListener('input', () => { inputs++; });

assert(box.checked === false, 'checkbox starts unchecked');
box.click();
assert(box.checked === true, 'click checks the checkbox');
assert(changes === 1, 'change fired on check (got ' + changes + ')');
assert(inputs === 1, 'input fired on check (got ' + inputs + ')');
box.click();
assert(box.checked === false, 'second click unchecks');
assert(changes === 2, 'change fired on uncheck (got ' + changes + ')');

// preventDefault suppresses the activation behavior, like a real click.
const box2 = document.createElement('input');
box2.setAttribute('type', 'checkbox');
root.appendChild(box2);
flush();
box2.addEventListener('click', (e) => e.preventDefault());
let box2changes = 0;
box2.addEventListener('change', () => { box2changes++; });
box2.click();
assert(box2.checked === false, 'preventDefault keeps the checkbox unchecked');
assert(box2changes === 0, 'no change event when the click is cancelled');

// ── radio: click checks it and unchecks the rest of its name group ──────────
const mk = (id) => {
  const r = document.createElement('input');
  r.setAttribute('type', 'radio');
  r.setAttribute('name', 'grp');
  r.id = id;
  root.appendChild(r);
  return r;
};
const r1 = mk('r1'), r2 = mk('r2');
flush();

let r2changes = 0;
r2.addEventListener('change', () => { r2changes++; });

r1.click();
assert(r1.checked === true, 'clicked radio is checked');
r2.click();
assert(r2.checked === true, 'second radio checked by click');
assert(r1.checked === false, 'first radio unchecked by group behavior');
assert(r2changes === 1, 'change fired on the clicked radio');

// Re-clicking a checked radio keeps it checked (radios don't toggle off).
r2.click();
assert(r2.checked === true, 'checked radio stays checked on re-click');

console.log('PASS');
