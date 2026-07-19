// Undo/redo for contenteditable, matching the input/textarea model:
// Ctrl+Z undoes, Ctrl+Y and Ctrl+Shift+Z redo. Runs of typed characters
// coalesce into one entry, as do runs of backspaces and runs of forward
// deletes; a caret move between edits breaks the run. Enter, paste, cut,
// typing over a selection and an IME commit are discrete and always stand
// alone. Undo restores the DOM *and* the selection.
//
// Entries are per-host: two contenteditable divs keep independent histories.
// When script mutates the tree under a pending entry the stack refuses to
// restore (and drops its history) rather than splicing into the wrong place.

const SDLK_RETURN = 13;
const SDLK_BACKSPACE = 8;
const SDLK_DELETE = 127;
const SDLK_Y = 121;
const SDLK_Z = 122;
const SDLK_LEFT = 0x40000050;
const KMOD_LSHIFT = 0x0001;
const KMOD_LCTRL = 0x0040;

const root = document.getElementById('root');

function press(key, mod) {
    keyDown(key, 0, mod || 0);
    keyUp(key, 0, mod || 0);
    flush();
}
function undo() { press(SDLK_Z, KMOD_LCTRL); }
function redoY() { press(SDLK_Y, KMOD_LCTRL); }
function redoShiftZ() { press(SDLK_Z, KMOD_LCTRL | KMOD_LSHIFT); }

function freshHost(html, id) {
    root.innerHTML =
        '<div id="' + (id || 'ed') + '" contenteditable="true" ' +
        'style="width:320px;height:60px;font-size:16px">' + (html || '') + '</div>';
    flush();
    return document.getElementById(id || 'ed');
}

// Put the caret at byte `off` of the host's first text node.
function caret(ed, off, node) {
    const sel = window.getSelection();
    sel.collapse(node || ed.firstChild, off);
    flush();
}
function selectRange(ed, a, b, node) {
    const sel = window.getSelection();
    const n = node || ed.firstChild;
    sel.setBaseAndExtent(n, a, n, b);
    flush();
}
// Click into a host to get a caret, the way a user does. An empty host has no
// text node to hit, so this exercises the element-position caret the press
// path establishes — seeding the caret from script instead would skip it.
function clickInto(ed) {
    const r = ed.getBoundingClientRect();
    mouseDown(r.left + 4, r.top + 4);
    mouseUp(r.left + 4, r.top + 4);
    flush();
}
function selOffsets() {
    const s = window.getSelection();
    return s.anchorOffset + ':' + s.focusOffset;
}

// --- Typing coalesces into one entry; undo/redo round-trips with caret -----
{
    const ed = freshHost('');
    clickInto(ed);             // empty host: the press path plants the caret
    textInput('a'); textInput('b'); textInput('c');
    flush();
    assert(ed.textContent === 'abc', 'typed abc, got: ' + JSON.stringify(ed.textContent));

    undo();
    assert(ed.textContent === '',
           'one undo reverts the whole typing run, got: ' + JSON.stringify(ed.textContent));

    redoY();
    assert(ed.textContent === 'abc',
           'Ctrl+Y redoes the run, got: ' + JSON.stringify(ed.textContent));
    assert(selOffsets() === '3:3', 'redo restores caret 3, got: ' + selOffsets());

    undo();
    assert(ed.textContent === '', 'undo after redo reverts again');
    redoShiftZ();
    assert(ed.textContent === 'abc', 'Ctrl+Shift+Z also redoes');
}

// --- A caret move between characters breaks the run ------------------------
{
    const ed = freshHost('');
    clickInto(ed);             // empty host: the press path plants the caret
    textInput('a'); textInput('b');
    flush();
    press(SDLK_LEFT);                 // caret move breaks coalescing
    textInput('c');
    flush();
    assert(ed.textContent === 'acb', 'typed a,b,Left,c -> acb, got: ' + JSON.stringify(ed.textContent));

    undo();
    assert(ed.textContent === 'ab', 'first undo removes only "c", got: ' + JSON.stringify(ed.textContent));
    undo();
    assert(ed.textContent === '', 'second undo removes "ab", got: ' + JSON.stringify(ed.textContent));
}

// --- Backspace runs coalesce; forward-delete runs coalesce separately ------
{
    const ed = freshHost('hello');
    caret(ed, 5);
    press(SDLK_BACKSPACE); press(SDLK_BACKSPACE);
    assert(ed.textContent === 'hel', 'two backspaces -> hel, got: ' + JSON.stringify(ed.textContent));
    undo();
    assert(ed.textContent === 'hello',
           'one undo restores the whole backspace run, got: ' + JSON.stringify(ed.textContent));
    assert(selOffsets() === '5:5', 'undo restores pre-backspace caret 5, got: ' + selOffsets());
}
{
    const ed = freshHost('hello');
    caret(ed, 0);
    press(SDLK_DELETE); press(SDLK_DELETE);
    assert(ed.textContent === 'llo', 'two deletes -> llo, got: ' + JSON.stringify(ed.textContent));
    undo();
    assert(ed.textContent === 'hello', 'one undo restores the delete run');
}

// --- Backspace and typing never merge into one entry -----------------------
{
    const ed = freshHost('ab');
    caret(ed, 2);
    press(SDLK_BACKSPACE);
    textInput('z');
    flush();
    assert(ed.textContent === 'az', 'backspace then type -> az, got: ' + JSON.stringify(ed.textContent));
    undo();
    assert(ed.textContent === 'a', 'undo removes only the typed z, got: ' + JSON.stringify(ed.textContent));
    undo();
    assert(ed.textContent === 'ab', 'undo restores the backspaced b');
}

// --- Enter is discrete: it stands alone between two typing runs ------------
{
    const ed = freshHost('');
    clickInto(ed);             // empty host: the press path plants the caret
    textInput('a');
    press(SDLK_RETURN);
    textInput('b');
    flush();
    assert(ed.querySelector('br') !== null, 'Enter inserted a <br>');
    assert(ed.textContent === 'ab', 'text is ab across the break, got: ' + JSON.stringify(ed.textContent));

    undo();
    assert(ed.textContent === 'a', 'undo removes the typed b, got: ' + JSON.stringify(ed.textContent));
    undo();
    assert(ed.querySelector('br') === null,
           'undo removes the <br>: ' + JSON.stringify(ed.innerHTML));
    assert(ed.textContent === 'a', 'text still a after undoing the break');
    undo();
    assert(ed.textContent === '', 'undo removes the typed a');

    redoY();
    assert(ed.textContent === 'a', 'redo re-types a');
    redoY();
    assert(ed.querySelector('br') !== null, 'redo re-inserts the <br>');
}

// --- Typing over a selection is discrete and restores the selection --------
{
    const ed = freshHost('hello world');
    selectRange(ed, 0, 5);
    textInput('X');
    flush();
    assert(ed.textContent === 'X world', 'typed over selection, got: ' + JSON.stringify(ed.textContent));
    undo();
    assert(ed.textContent === 'hello world',
           'undo restores the replaced text, got: ' + JSON.stringify(ed.textContent));
    assert(selOffsets() === '0:5', 'undo restores the replaced selection, got: ' + selOffsets());
}

// --- Paste and cut are discrete ------------------------------------------
{
    const ed = freshHost('hello');
    caret(ed, 5);
    paste('!!');
    flush();
    assert(ed.textContent === 'hello!!', 'pasted into CE, got: ' + JSON.stringify(ed.textContent));
    undo();
    assert(ed.textContent === 'hello', 'undo reverts the paste, got: ' + JSON.stringify(ed.textContent));
}
{
    const ed = freshHost('hello world');
    selectRange(ed, 0, 6);
    const cutText = cut();
    flush();
    assert(cutText === 'hello ', 'cut returned the selection, got: ' + JSON.stringify(cutText));
    assert(ed.textContent === 'world', 'cut removed it, got: ' + JSON.stringify(ed.textContent));
    undo();
    assert(ed.textContent === 'hello world', 'undo restores the cut text');
    assert(selOffsets() === '0:6', 'undo restores the cut selection, got: ' + selOffsets());
}

// --- IME commit is one discrete entry -------------------------------------
{
    const ed = freshHost('hi ');
    caret(ed, 3);
    imeCompose('ni', 2);
    imeCompose('nih', 3);
    imeCommit('你好');
    flush();
    assert(ed.textContent === 'hi 你好',
           'IME committed, got: ' + JSON.stringify(ed.textContent));
    undo();
    assert(ed.textContent === 'hi ',
           'one undo removes the whole committed run, got: ' + JSON.stringify(ed.textContent));
    redoY();
    assert(ed.textContent === 'hi 你好', 'redo re-applies the commit');
}

// --- IME cancel resurrects the selection it replaced ----------------------
{
    const ed = freshHost('hello world');
    selectRange(ed, 0, 5);
    imeCompose('ab', 2);
    flush();
    assert(ed.textContent === 'ab world',
           'composition replaced the selection, got: ' + JSON.stringify(ed.textContent));
    imeCancel();
    flush();
    assert(ed.textContent === 'hello world',
           'cancel resurrects the replaced selection, got: ' + JSON.stringify(ed.textContent));
    assert(selOffsets() === '0:5',
           'cancel restores the selection too, got: ' + selOffsets());
    undo();
    assert(ed.textContent === 'hello world',
           'a canceled composition leaves no undo entry, got: ' +
           JSON.stringify(ed.textContent));
}

// --- Each host keeps its own history --------------------------------------
{
    root.innerHTML =
        '<div id="e1" contenteditable="true" style="width:200px;height:40px">one</div>' +
        '<div id="e2" contenteditable="true" style="width:200px;height:40px">two</div>';
    flush();
    const a = document.getElementById('e1');
    const b = document.getElementById('e2');

    caret(a, 3, a.firstChild); textInput('A'); flush();
    caret(b, 3, b.firstChild); textInput('B'); flush();
    assert(a.textContent === 'oneA' && b.textContent === 'twoB', 'both hosts edited');

    undo();
    assert(b.textContent === 'two', 'undo in host b reverts b, got: ' + JSON.stringify(b.textContent));
    assert(a.textContent === 'oneA', 'host a is untouched, got: ' + JSON.stringify(a.textContent));

    caret(a, 4, a.firstChild);
    undo();
    assert(a.textContent === 'one', 'host a still has its own history, got: ' + JSON.stringify(a.textContent));
}

// --- Script mutating under a pending entry: refuse, do not corrupt ---------
{
    const ed = freshHost('hello');
    caret(ed, 5);
    textInput('!');
    flush();
    assert(ed.textContent === 'hello!', 'typed !');

    ed.innerHTML = 'totally different';   // script rewrites the host
    flush();

    undo();
    assert(ed.textContent === 'totally different',
           'undo refuses to splice into a script-rewritten host, got: ' +
           JSON.stringify(ed.textContent));
    undo();
    assert(ed.textContent === 'totally different',
           'the whole history was dropped, not partially applied');
}

// --- Empty-stack undo and spent redo are no-ops ---------------------------
{
    const ed = freshHost('x');
    caret(ed, 1);
    undo();
    assert(ed.textContent === 'x', 'undo on an empty history is a no-op');
    redoY();
    assert(ed.textContent === 'x', 'redo with nothing undone is a no-op');
}

console.log('contenteditable undo/redo: OK');
