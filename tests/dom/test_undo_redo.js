// Undo/redo for input/textarea text editing: Ctrl+Z undoes, Ctrl+Y and
// Ctrl+Shift+Z redo. Consecutive typed characters coalesce into one entry;
// caret/selection moves, pastes, cuts, and selection replaces break or stand
// apart. Undo restores text AND selection; programmatic `.value =` writes
// clear the element's history. Empty-stack undo / spent redo are no-ops.

const SDLK_RETURN = 13;
const SDLK_Y = 121;
const SDLK_Z = 122;
const SDLK_LEFT = 0x40000050;
const SDLK_END = 0x4000004d;
const KMOD_LSHIFT = 0x0001;
const KMOD_LCTRL = 0x0040;

const root = document.getElementById('root');

function press(key, mod) {
    keyDown(key, 0, mod || 0);
    keyUp(key, 0, mod || 0);
}
function undo() { press(SDLK_Z, KMOD_LCTRL); }
function redoY() { press(SDLK_Y, KMOD_LCTRL); }
function redoShiftZ() { press(SDLK_Z, KMOD_LCTRL | KMOD_LSHIFT); }

function freshInput(id) {
    root.innerHTML = '<input id="' + id + '" type="text">';
    flush();
    const el = document.getElementById(id);
    const r = el.getBoundingClientRect();
    click(r.left + 5, r.top + r.height / 2);
    return el;
}

// --- Type + undo + redo round-trip, with caret asserts ---------------------
{
    const el = freshInput('in1');
    textInput('a'); textInput('b'); textInput('c');
    assert(el.value === 'abc', 'typed abc, got: ' + el.value);
    assert(el.selectionStart === 3 && el.selectionEnd === 3,
           'caret after typing at 3, got: ' + el.selectionStart);

    undo();
    assert(el.value === '', 'one undo reverts the whole coalesced run, got: "' + el.value + '"');
    assert(el.selectionStart === 0 && el.selectionEnd === 0,
           'undo restores pre-typing caret 0, got: ' + el.selectionStart);

    redoY();
    assert(el.value === 'abc', 'Ctrl+Y redo restores abc, got: "' + el.value + '"');
    assert(el.selectionStart === 3 && el.selectionEnd === 3,
           'redo restores post-typing caret 3, got: ' + el.selectionStart);

    undo();
    assert(el.value === '', 'undo after redo reverts again, got: "' + el.value + '"');
    redoShiftZ();
    assert(el.value === 'abc', 'Ctrl+Shift+Z also redoes, got: "' + el.value + '"');
}

// --- Coalesce break on caret move: type "ab", Left, type "c" ----------------
{
    const el = freshInput('in2');
    textInput('a'); textInput('b');
    press(SDLK_LEFT);                       // caret 2 -> 1, breaks the run
    textInput('c');
    assert(el.value === 'acb', 'insert at moved caret, got: ' + el.value);

    undo();
    assert(el.value === 'ab', 'first undo removes only "c", got: "' + el.value + '"');
    assert(el.selectionStart === 1, 'undo restores caret before "c", got: ' + el.selectionStart);
    undo();
    assert(el.value === '', 'second undo removes "ab", got: "' + el.value + '"');
}

// --- Backspace run coalesces; undo restores it in one step -----------------
{
    const el = freshInput('in3');
    textInput('wxyz');
    press(8); press(8);                     // two backspaces -> one entry
    assert(el.value === 'wx', 'backspaces deleted, got: ' + el.value);
    undo();
    assert(el.value === 'wxyz', 'one undo restores both backspaced chars, got: "' + el.value + '"');
    assert(el.selectionStart === 4, 'caret back at 4, got: ' + el.selectionStart);
    undo();
    assert(el.value === '', 'next undo removes the typed text, got: "' + el.value + '"');
}

// --- Selection replace: undo restores replaced text AND the selection ------
{
    const el = freshInput('in4');
    textInput('hello world');
    el.setSelectionRange(6, 11);            // "world"
    textInput('there');
    assert(el.value === 'hello there', 'typed over selection, got: ' + el.value);

    undo();
    assert(el.value === 'hello world', 'undo restores the replaced text, got: "' + el.value + '"');
    assert(el.selectionStart === 6 && el.selectionEnd === 11,
           'undo restores the selection 6..11, got: ' +
           el.selectionStart + '..' + el.selectionEnd);

    redoY();
    assert(el.value === 'hello there', 'redo re-applies the replace, got: "' + el.value + '"');
    assert(el.selectionStart === 11 && el.selectionEnd === 11,
           'redo restores post-replace caret 11, got: ' + el.selectionStart);
}

// --- Paste is a discrete entry ----------------------------------------------
{
    const el = freshInput('in5');
    textInput('abc');
    paste('XYZ');
    assert(el.value === 'abcXYZ', 'paste appended, got: ' + el.value);
    undo();
    assert(el.value === 'abc', 'undo removes only the paste, got: "' + el.value + '"');
    undo();
    assert(el.value === '', 'next undo removes the typing, got: "' + el.value + '"');
}

// --- Cut is a discrete, undoable entry --------------------------------------
{
    const el = freshInput('in6');
    textInput('cutme');
    el.setSelectionRange(0, 3);
    const cutText = cut();
    assert(cutText === 'cut', 'cut returned the selection, got: "' + cutText + '"');
    assert(el.value === 'me', 'cut removed the selection, got: ' + el.value);
    undo();
    assert(el.value === 'cutme', 'undo restores the cut text, got: "' + el.value + '"');
    assert(el.selectionStart === 0 && el.selectionEnd === 3,
           'undo restores the cut selection 0..3, got: ' +
           el.selectionStart + '..' + el.selectionEnd);
}

// --- Programmatic .value= clears history ------------------------------------
{
    const el = freshInput('in7');
    textInput('typed');
    el.value = 'script';
    undo();
    assert(el.value === 'script', '.value= cleared history; undo is a no-op, got: "' + el.value + '"');
    redoY();
    assert(el.value === 'script', 'redo after clear is a no-op too, got: "' + el.value + '"');
}

// --- Empty-stack undo / spent redo are safe no-ops ---------------------------
{
    const el = freshInput('in8');
    undo(); undo();
    assert(el.value === '', 'undo on empty history is a no-op');
    redoY();
    assert(el.value === '', 'redo on empty history is a no-op');
    textInput('a');
    undo();
    textInput('b');                          // new edit clears the redo tail
    redoY();
    assert(el.value === 'b', 'redo after a fresh edit is a no-op, got: "' + el.value + '"');
}

// --- input events carry historyUndo / historyRedo ---------------------------
{
    const el = freshInput('in9');
    const types = [];
    el.addEventListener('input', e => types.push(e.inputType));
    textInput('q');
    undo();
    redoY();
    assert(types.join(',') === 'insertText,historyUndo,historyRedo',
           'inputType sequence, got: ' + types.join(','));
}

// --- Textarea: multi-line undo across a newline insertion -------------------
{
    root.innerHTML = '<textarea id="ta1" rows="4" cols="20"></textarea>';
    flush();
    const ta = document.getElementById('ta1');
    const r = ta.getBoundingClientRect();
    click(r.left + 5, r.top + 5);

    textInput('ab');
    press(SDLK_RETURN);
    textInput('cd');
    assert(ta.value === 'ab\ncd', 'textarea multi-line typed, got: ' + JSON.stringify(ta.value));

    undo();
    assert(ta.value === 'ab\n', 'undo removes "cd", got: ' + JSON.stringify(ta.value));
    undo();
    assert(ta.value === 'ab', 'undo removes the newline, got: ' + JSON.stringify(ta.value));
    assert(ta.selectionStart === 2, 'caret back at 2, got: ' + ta.selectionStart);
    undo();
    assert(ta.value === '', 'undo removes "ab", got: ' + JSON.stringify(ta.value));

    redoY(); redoY(); redoY();
    assert(ta.value === 'ab\ncd', 'three redos rebuild the textarea, got: ' + JSON.stringify(ta.value));
    assert(ta.selectionStart === 5, 'caret restored to 5, got: ' + ta.selectionStart);
}

// --- Textarea: initial HTML content undoes back to it -----------------------
{
    root.innerHTML = '<textarea id="ta2" rows="3" cols="20">seed</textarea>';
    flush();
    const ta = document.getElementById('ta2');
    const r = ta.getBoundingClientRect();
    click(r.left + 5, r.top + 5);
    press(SDLK_END);
    textInput('ling');
    assert(ta.value === 'seedling', 'typed onto initial content, got: ' + ta.value);
    undo();
    assert(ta.value === 'seed', 'undo restores the initial HTML content, got: "' + ta.value + '"');
}
