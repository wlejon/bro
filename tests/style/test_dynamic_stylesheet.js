// A <style> element inserted at runtime — the CSS-in-JS pattern
// (document.createElement('style') + head.appendChild) — must have its rules
// picked up by the cascade, not ignored until the next full document parse.
// Regression test for dynamic-stylesheet reconcile in resolveStyles().

const root = document.getElementById('root');

// 1. Inject a stylesheet dynamically, then an element that matches its rule.
const style = document.createElement('style');
style.textContent = '.dyn-box { width: 321px; height: 123px; }';
document.head.appendChild(style);

const box = document.createElement('div');
box.className = 'dyn-box';
root.appendChild(box);

flush();
let r = box.getBoundingClientRect();
assert(Math.round(r.width) === 321, 'dynamic <style> width applied (got ' + r.width + ')');
assert(Math.round(r.height) === 123, 'dynamic <style> height applied (got ' + r.height + ')');

// 2. A second dynamic sheet adds rules WITHOUT dropping the first (incremental,
//    no cascade clear).
const style2 = document.createElement('style');
style2.textContent = '.dyn-box2 { width: 210px; }';
document.head.appendChild(style2);

const box2 = document.createElement('div');
box2.className = 'dyn-box2';
root.appendChild(box2);

flush();
assert(Math.round(box2.getBoundingClientRect().width) === 210, 'second dynamic sheet applied');
assert(Math.round(box.getBoundingClientRect().width) === 321, 'first dynamic sheet still applied');

// 3. Setting the text on a <style> BEFORE it is connected, then appending it,
//    still registers it (the order the CSS-in-JS helper uses).
const style3 = document.createElement('style');
style3.textContent = '.dyn-box3 { width: 150px; }';
const box3 = document.createElement('div');
box3.className = 'dyn-box3';
root.appendChild(box3);
document.head.appendChild(style3);   // connected after its text was set
flush();
assert(Math.round(box3.getBoundingClientRect().width) === 150, 'set-text-then-append registers');

console.log('PASS dynamic stylesheet');
