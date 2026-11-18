// A real mouse click, the keyboard and element.click() take one activation
// path (HTML's activation behaviour, engine/element_activation.cpp), as in
// Chromium: a checkbox or radio changes when its click is dispatched — not at
// the press — and fires input and change after the click; a cancelled click
// puts it back. The same for a label's text, Space on a focused checkbox,
// Enter / Space on a button (submit, with the submitter, and requestSubmit),
// Enter in a field (implicit submission), a link (followed unless
// prevented), a <summary>'s disclosure and a <select>'s change.

document.body.innerHTML = `
  <style>body { margin: 0; font: 16px sans-serif } input, button, a, summary, label, select { display: block; margin: 4px 0 }</style>
  <input type="checkbox" id="cb" style="width:20px;height:20px">
  <input type="radio" name="g" id="r1" checked style="width:20px;height:20px">
  <input type="radio" name="g" id="r2" style="width:20px;height:20px">
  <label id="lab" style="width:200px;height:24px"><span id="labtext">Label text</span> <input type="checkbox" id="lcb"></label>
  <form id="f"><input id="field" name="q" value="x" style="width:120px;height:20px"><button id="sub" style="width:80px;height:24px">Go</button></form>
  <details id="det"><summary id="sum" style="width:200px;height:20px">More</summary><p>hidden</p></details>
  <a id="link" href="#landed" style="width:100px;height:20px">link</a>
  <select id="sel" style="width:120px;height:24px"><option>a</option><option>b</option></select>
`;
flush();

const log = [];
for (const id of ['cb', 'r1', 'r2', 'lcb']) {
    const el = document.getElementById(id);
    for (const type of ['mousedown', 'mouseup', 'click', 'input', 'change']) {
        el.addEventListener(type, () => log.push(id + ':' + type + ':' + el.checked));
    }
}
function center(el) {
    const r = el.getBoundingClientRect();
    return [r.left + r.width / 2, r.top + r.height / 2];
}
function press(el) {
    const [x, y] = center(el);
    mouseDown(x, y);
    advanceTime(16);
}
function release(el) {
    const [x, y] = center(el);
    mouseUp(x, y);
    advanceTime(16);
}

// ---- checkbox: nothing changes at the press -----------------------------------
const cb = document.getElementById('cb');
press(cb);
assert(cb.checked === false, 'a press does not toggle the checkbox');
assert(log.join() === 'cb:mousedown:false', 'only mousedown at the press, got ' + log.join());
release(cb);
assert(cb.checked === true, 'the click toggles it');
assert(log.join() === 'cb:mousedown:false,cb:mouseup:false,cb:click:true,cb:input:true,cb:change:true',
       'mouseup, then click with the new state, then input and change, got ' + log.join());

// A press on the box and a release elsewhere makes no click: nothing changes.
log.length = 0;
press(cb);
release(document.getElementById('sum'));
assert(cb.checked === true && !log.includes('cb:change:false'), 'no click, no change: ' + log.join());

// A cancelled click puts it back, with no input or change.
log.length = 0;
const cancel = (e) => e.preventDefault();
cb.addEventListener('click', cancel);
press(cb);
release(cb);
assert(cb.checked === true, 'a cancelled click leaves it checked');
assert(log.join() === 'cb:mousedown:true,cb:mouseup:true,cb:click:false',
       'the click saw the toggled state, nothing after, got ' + log.join());
cb.removeEventListener('click', cancel);

// ---- radio --------------------------------------------------------------------------
log.length = 0;
const r1 = document.getElementById('r1');
const r2 = document.getElementById('r2');
press(r2);
assert(r2.checked === false && r1.checked === true, 'a press leaves the radio group alone');
release(r2);
assert(r2.checked === true && r1.checked === false, 'the click checks r2 and unchecks r1');
assert(log.join() === 'r2:mousedown:false,r2:mouseup:false,r2:click:true,r2:input:true,r2:change:true',
       'radio events, got ' + log.join());
log.length = 0;
press(r2);
release(r2);
assert(log.join() === 'r2:mousedown:true,r2:mouseup:true,r2:click:true',
       'clicking a checked radio fires no input or change, got ' + log.join());

// ---- label text: forwarded click, at the release ----------------------------------
log.length = 0;
const lcb = document.getElementById('lcb');
const labtext = document.getElementById('labtext');
press(labtext);
assert(lcb.checked === false, 'pressing the label text does not toggle');
release(labtext);
assert(lcb.checked === true, 'clicking the label text toggles its checkbox');
assert(log.join() === 'lcb:click:true,lcb:input:true,lcb:change:true', 'label forward, got ' + log.join());
assert(document.activeElement === lcb, 'the label focused its control');
// label.click() does the same.
log.length = 0;
document.getElementById('lab').click();
assert(lcb.checked === false && log.join() === 'lcb:click:false,lcb:input:false,lcb:change:false',
       'label.click() forwards, got ' + log.join());

// ---- keyboard: Space clicks a focused checkbox at its keyup -------------------------
log.length = 0;
cb.focus();
keyDown('Space');
advanceTime(16);
assert(cb.checked === true, 'Space keydown does not toggle yet');
keyUp('Space');
advanceTime(16);
assert(cb.checked === false, 'Space keyup clicks it');
assert(log.join() === 'cb:click:false,cb:input:false,cb:change:false', 'Space click events, got ' + log.join());
// A cancelled keydown arms nothing.
log.length = 0;
const stopKey = (e) => e.preventDefault();
cb.addEventListener('keydown', stopKey);
keyDown('Space');
keyUp('Space');
advanceTime(16);
assert(cb.checked === false && log.length === 0, 'a cancelled Space keydown clicks nothing');
cb.removeEventListener('keydown', stopKey);

// ---- form submission ----------------------------------------------------------------
const form = document.getElementById('f');
const sub = document.getElementById('sub');
const submits = [];
form.addEventListener('submit', (e) => { e.preventDefault(); submits.push(e.submitter ? e.submitter.id : null); });
const subClicks = [];
sub.addEventListener('click', (e) => subClicks.push(e.isTrusted));
press(sub);
release(sub);
assert(submits.join() === 'sub', 'a mouse click on the submit button submits with it, got ' + submits.join());
sub.focus();
keyDown('Enter');
keyUp('Enter');
advanceTime(16);
assert(submits.join() === 'sub,sub', 'Enter on the button submits, got ' + submits.join());
keyDown('Space');
keyUp('Space');
advanceTime(16);
assert(submits.join() === 'sub,sub,sub', 'Space on the button submits, got ' + submits.join());
assert(subClicks.join() === 'true,true,true', 'each was a trusted click, got ' + subClicks.join());
document.getElementById('field').focus();
keyDown('Enter');
keyUp('Enter');
advanceTime(16);
assert(submits.join() === 'sub,sub,sub,sub', 'Enter in the field clicks the default button, got ' + submits.join());
form.requestSubmit(sub);
assert(submits.length === 5 && submits[4] === 'sub', 'requestSubmit(sub) fires submit with the submitter');
form.requestSubmit();
assert(submits.length === 6 && submits[5] === null, 'requestSubmit() fires submit with no submitter');
sub.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true }));
assert(submits.length === 7, 'a dispatched click runs the activation behaviour too');

// ---- details / summary ------------------------------------------------------------
const det = document.getElementById('det');
let toggles = 0;
det.addEventListener('toggle', () => toggles++);
press(document.getElementById('sum'));
assert(!det.open, 'a press does not open the details');
release(document.getElementById('sum'));
assert(det.open && toggles === 1, 'the click opens it and fires toggle');
document.getElementById('sum').focus();
keyDown('Enter');
keyUp('Enter');
advanceTime(16);
assert(!det.open, 'Enter on the summary closes it');

// ---- a link is followed unless the click is cancelled ------------------------------
const link = document.getElementById('link');
const stopLink = (e) => e.preventDefault();
link.addEventListener('click', stopLink);
press(link);
release(link);
assert(location.hash === '', 'a cancelled link click does not navigate, hash ' + location.hash);
link.removeEventListener('click', stopLink);
let hashes = 0;
window.addEventListener('hashchange', () => hashes++);
press(link);
release(link);
advanceTime(16);
assert(location.hash === '#landed' && hashes === 1, 'the link click navigates to its fragment, hash ' + location.hash);

// ---- select: change after a pick ----------------------------------------------------
const sel = document.getElementById('sel');
const selEvents = [];
sel.addEventListener('input', () => selEvents.push('input:' + sel.value));
sel.addEventListener('change', () => selEvents.push('change:' + sel.value));
sel.focus();
keyDown('ArrowDown');
keyUp('ArrowDown');
advanceTime(16);
assert(sel.value === 'b', 'ArrowDown on a closed select picks the next option, got ' + sel.value);
assert(selEvents.join() === 'input:b,change:b', 'and fires input then change, got ' + selEvents.join());

console.log('test_user_activation: OK');
