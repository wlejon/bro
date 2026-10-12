// <input list=...> with a <datalist>: input.list, datalist.options, and the
// suggestions list the engine shows under the field (engine/input_datalist.cpp).
// Typing shows the options holding the text (value or label, case aside);
// Down and Up move through them and Enter takes one, a click takes one, and a
// take replaces the text and fires `input` (insertReplacementText) with the
// field still focused. Escape, Tab, a press elsewhere and blur close it; a
// script's value write does not open it; Down on the field opens it.

document.body.style.background = 'rgb(40, 40, 40)';
const root = document.getElementById('root');
root.innerHTML =
  '<input type="text" id="f" list="birds" style="font:16px Arial">' +
  '<datalist id="birds">' +
  '<option value="Mallard"></option>' +
  '<option value="Mandarin Duck"></option>' +
  '<option value="Blue Jay" label="Cyanocitta cristata"></option>' +
  '<option value="Mute Swan" disabled></option>' +
  '<option value=""></option>' +
  '</datalist>' +
  '<div style="height:240px"></div>' +
  '<input type="text" id="g">' +
  '<input type="text" id="h" list="nothing">' +
  '<div id="nothing"></div>';
flush();

const f = document.getElementById('f');
const g = document.getElementById('g');
const h = document.getElementById('h');
const birds = document.getElementById('birds');

// The DOM side.
assert(birds instanceof HTMLDataListElement, 'a <datalist> is an HTMLDataListElement');
assert(f.list === birds, 'input.list is the datalist its list attribute names');
assert(g.list === null, 'no list attribute: null');
assert(h.list === null, 'a list naming something other than a datalist: null');
assert(birds.options.length === 5, 'datalist.options holds every option, got ' + birds.options.length);
assert(birds.options[2].value === 'Blue Jay', 'and they are the option elements');
assert(birds.getBoundingClientRect().height === 0, 'a datalist is not rendered');

const box = () => f.getBoundingClientRect();
const rowY = (i) => box().bottom + 1 + 12 + i * 26;   // inside row i (rows are ~26px at 16px)
const isOpen = () => {
  const r = box();
  const p = getPixel(r.right - 8, rowY(0));
  return p.r > 200 && p.g > 200 && p.b > 200;
};
const log = [];
f.addEventListener('input', (e) => log.push(`input:${e.inputType}:${f.value}`));
f.addEventListener('keydown', (e) => log.push(`keydown:${e.key}`));

assert(!isOpen(), 'closed at first');

// A script's write does not open it.
f.focus();
f.value = 'ma';
flush();
assert(!isOpen(), 'a script\'s value write shows no suggestions');
f.value = '';

// Typing shows what matches; Down twice and Enter takes the second.
textInput('m');
textInput('a');
flush();
assert(isOpen(), 'typing "ma" shows the suggestions');
log.length = 0;
keyDown('ArrowDown'); keyUp('ArrowDown');
keyDown('ArrowDown'); keyUp('ArrowDown');
keyDown('Enter'); keyUp('Enter');
flush();
assert(f.value === 'Mandarin Duck', 'Down, Down, Enter takes the second match, got ' + f.value);
assert(log.join() === 'input:insertReplacementText:Mandarin Duck',
       'one input event, insertReplacementText; the list\'s keys are its own: ' + log.join());
assert(document.activeElement === f, 'the field keeps the focus');
assert(!isOpen(), 'a take closes the list');

// Undo puts the typed text back: a take is one edit.
keyDown('KeyZ', 0, 0x0040); keyUp('KeyZ', 0, 0x0040);
flush();
assert(f.value === 'ma', 'Ctrl+Z undoes the take, got ' + f.value);

// Escape closes it and leaves the text.
f.value = '';
textInput('j');
flush();
assert(isOpen(), '"j" matches Blue Jay');
keyDown('Escape'); keyUp('Escape');
flush();
assert(!isOpen(), 'Escape closes the list');
assert(f.value === 'j' && document.activeElement === f, 'and leaves the field as it was');

// A label matches too, and a click on a row takes it.
f.value = '';
textInput('cyan');
flush();
assert(isOpen(), 'the label "Cyanocitta cristata" matches "cyan"');
log.length = 0;
click(box().left + 20, rowY(0));
flush();
assert(f.value === 'Blue Jay', 'a click takes the row, got ' + f.value);
assert(log.join() === 'input:insertReplacementText:Blue Jay', 'input fires: ' + log.join());
assert(document.activeElement === f, 'and the field keeps the focus');
assert(!isOpen(), 'closed after the click');

// Down on the field opens every suggestion; disabled and empty ones are not
// suggestions. Up from none is the last; Enter on none goes to the page.
f.value = '';
keyDown('ArrowDown'); keyUp('ArrowDown');
flush();
assert(isOpen(), 'Down opens the list on an empty field');
const third = getPixel(box().right - 8, rowY(2));
const fourth = getPixel(box().right - 8, rowY(3));
assert(third.r > 200, 'three suggestions: the third row is in the list');
assert(fourth.r < 100, 'no fourth: a disabled option and an empty value suggest nothing');
keyDown('ArrowUp'); keyUp('ArrowUp');
keyDown('Enter'); keyUp('Enter');
flush();
assert(f.value === 'Blue Jay', 'Up from none highlights the last, got ' + f.value);
f.value = '';
textInput('m');
flush();
assert(isOpen(), 'open again');
log.length = 0;
keyDown('Enter'); keyUp('Enter');
flush();
assert(!isOpen(), 'Enter with nothing highlighted closes it');
assert(log.includes('keydown:Enter'), 'and goes on to the page: ' + log.join());

// The text that only spells one option needs no suggestion.
f.value = 'Mallar';
textInput('d');
flush();
assert(!isOpen(), 'no suggestion for the option the field already spells');

// A press elsewhere closes the list and goes on to what was pressed.
f.value = '';
textInput('m');
flush();
assert(isOpen(), 'open');
const gr = g.getBoundingClientRect();
let gDown = 0;
g.addEventListener('mousedown', () => gDown++);
click(gr.left + 5, gr.top + gr.height / 2);
flush();
assert(gDown === 1 && document.activeElement === g, 'the press reaches the other field and focuses it');
assert(!isOpen(), 'and the list is closed');

// Losing the focus closes it.
f.focus();
f.value = '';
textInput('m');
flush();
assert(isOpen(), 'open');
f.blur();
flush();
assert(!isOpen(), 'blur closes the list');

// Tab closes it and moves on.
f.focus();
f.value = '';
textInput('m');
flush();
assert(isOpen(), 'open');
keyDown('Tab'); keyUp('Tab');
flush();
assert(!isOpen(), 'Tab closes the list');
assert(document.activeElement !== f, 'and moves the focus on');

// The page hovers as usual away from the list while it is open.
f.focus();
f.value = '';
textInput('m');
flush();
assert(isOpen(), 'open');
let over = 0;
g.addEventListener('mouseover', () => over++);
mouseMove(gr.left + 5, gr.top + gr.height / 2);
flush();
assert(over >= 1, 'the page still sees the pointer outside the list');
keyDown('Escape'); keyUp('Escape');
