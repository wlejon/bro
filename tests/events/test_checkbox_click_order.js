// element.click() on a checkbox or radio: the box changes before the click is
// dispatched (legacy-pre-activation), so click listeners read the new value;
// a cancelled click restores it; otherwise input then change follow, as in
// Chromium. React's checkbox onChange is a click listener reading `checked`.
const log = [];
const box = document.createElement('input');
box.type = 'checkbox';
document.body.appendChild(box);
for (const t of ['click', 'input', 'change']) box.addEventListener(t, (e) => log.push(`${t}:${box.checked}:${e.isTrusted}`));
box.click();
assert(log.join(' ') === 'click:true:false input:true:true change:true:true', 'checkbox order: ' + log.join(' '));
assert(box.checked === true, 'checked after click');

log.length = 0;
const cancel = (e) => e.preventDefault();
box.addEventListener('click', cancel);
box.click();
assert(log.join(' ') === 'click:false:false', 'cancelled click sees the toggle, then no events: ' + log.join(' '));
assert(box.checked === true, 'cancelled click restores checked');
box.removeEventListener('click', cancel);

const mk = () => { const r = document.createElement('input'); r.type = 'radio'; r.name = 'g'; document.body.appendChild(r); return r; };
const a = mk(), b = mk();
a.click();
log.length = 0;
b.addEventListener('click', (e) => { log.push(`click:${a.checked}${b.checked}`); e.preventDefault(); });
b.click();
assert(log.join(' ') === 'click:falsetrue', 'radio click sees the new group state: ' + log.join(' '));
assert(a.checked && !b.checked, 'cancelled radio click restores the previous one');

const d = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'checked');
assert(d && typeof d.get === 'function' && typeof d.set === 'function', 'checked is an accessor on HTMLInputElement.prototype');
const v = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value');
assert(v && typeof v.set === 'function', 'value is an accessor on HTMLInputElement.prototype');
assert(Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value'), 'textarea value accessor');
assert(Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype, 'selectedIndex'), 'select selectedIndex accessor');
const div = document.createElement('div');
div.value = 7;
assert(div.value === 7, 'a div value is still an expando');
console.log('test_checkbox_click_order: OK');
