// Textarea .value must reflect: (a) initial HTML content, (b) values set via
// the JS setter, (c) typed text — all read back through the same getter.
// Regression: typing wrote to the "value" attribute but the JS getter read
// textContent, so typed text was invisible to JS.

const SDLK_END = 0x4000004d;

const root = document.getElementById('root');

// --- Initial HTML content shows through .value ---------------------------
root.innerHTML = '<textarea id="ta1">hello</textarea>';
flush();
const ta1 = document.getElementById('ta1');
assert(ta1.value === 'hello', 'initial HTML content reads via .value, got: ' + ta1.value);

// --- JS setter, then JS getter ------------------------------------------
ta1.value = 'world';
assert(ta1.value === 'world', '.value reads back what was set, got: ' + ta1.value);

// --- Typing reflects in .value (no getAttribute workaround needed) -------
root.innerHTML = '<textarea id="ta2" rows="3" cols="20"></textarea>';
flush();
const ta2 = document.getElementById('ta2');
const r = ta2.getBoundingClientRect();
click(r.left + 10, r.top + 10);
textInput('typed');
assert(ta2.value === 'typed', 'typed text reads through .value, got: ' + ta2.value);

// --- Set then type: typing extends the set value -------------------------
root.innerHTML = '<textarea id="ta3" rows="3" cols="20"></textarea>';
flush();
const ta3 = document.getElementById('ta3');
ta3.value = 'pre';
const r3 = ta3.getBoundingClientRect();
click(r3.left + 10, r3.top + 10);
// cursor lands somewhere in the value; press End to ensure append
keyDown(SDLK_END); keyUp(SDLK_END);
textInput('fix');
assert(ta3.value === 'prefix', 'typing after set extends the value, got: ' + ta3.value);
