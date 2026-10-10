// :focus-visible follows the browsers' heuristic (Selectors 4 §9.4): focus
// that arrives by keyboard shows the indicator, a click that focuses a button
// does not, a text field always does, and a key pressed while a clicked
// button has focus shows it from then on. Document::focusVisible.

const root = document.getElementById('root');
root.innerHTML =
    '<button id="btn" style="position:absolute;left:10px;top:10px;width:120px;height:30px">Button</button>' +
    '<button id="btn2" style="position:absolute;left:10px;top:60px;width:120px;height:30px">Other</button>' +
    '<input id="field" type="text" style="position:absolute;left:10px;top:110px;width:120px;height:24px">';
flush();

const btn = document.getElementById('btn');
const btn2 = document.getElementById('btn2');
const field = document.getElementById('field');
const clickOn = (el) => {
    const r = el.getBoundingClientRect();
    click(r.left + r.width / 2, r.top + r.height / 2);
    advanceTime(16);
};

// Script focus before any user input shows the indicator, as in browsers.
btn.focus();
assert(btn.matches(':focus-visible'), 'script focus before any input is visible');

// A click on a button focuses it without the indicator.
clickOn(btn2);
assert(document.activeElement === btn2, 'the click focused the button');
assert(btn2.matches(':focus'), 'the clicked button matches :focus');
assert(!btn2.matches(':focus-visible'), 'a clicked button is not :focus-visible');

// Script focus right after a click follows the pointer modality.
btn.focus();
assert(!btn.matches(':focus-visible'), 'script focus after a click is not visible');

// A key pressed while it has focus shows the indicator.
keyDown(0x40000051); keyUp(0x40000051);   // SDLK_DOWN
advanceTime(16);
assert(btn.matches(':focus-visible'), 'a key press makes the focused button visible');

// A modifier alone does not count as keyboard use.
clickOn(btn2);
keyDown(0x400000E0); keyUp(0x400000E0);   // SDLK_LCTRL
advanceTime(16);
assert(!btn2.matches(':focus-visible'), 'a modifier alone does not show the indicator');

// Tab moves focus by keyboard: visible.
keyDown(0x09); keyUp(0x09);               // SDLK_TAB
advanceTime(16);
assert(document.activeElement !== btn2, 'Tab moved focus');
assert(document.activeElement.matches(':focus-visible'), 'focus moved by Tab is visible');

// A text field shows its focus even when clicked.
clickOn(field);
assert(document.activeElement === field, 'the click focused the field');
assert(field.matches(':focus-visible'), 'a clicked text field is :focus-visible');

console.log('test_focus_visible PASSED');
