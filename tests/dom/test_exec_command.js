// document.execCommand() and its query companions.
//
// The point of these tests is not that each command has *an* effect, but that
// a scripted command and the key press it names produce the SAME effect. Both
// go through one set of edit primitives in input_handling.cpp precisely so
// they cannot drift; a test that only checked execCommand's output would pass
// just as happily against a second, divergent implementation. So wherever a
// keyboard equivalent exists, the assertion compares the two directly.
//
// Commands this build does not implement (bold, italic, …) must report
// unsupported rather than silently doing nothing — contenteditable here is
// plaintext-v1 with no inline-formatting model, and a caller needs to be able
// to feature-detect that instead of discovering it from a no-op.

const SDLK_BACKSPACE = 8;
const SDLK_DELETE = 127;
const SDLK_RETURN = 13;
const SDLK_Z = 122;
const SDLK_Y = 121;
const KMOD_LCTRL = 0x0040;

const root = document.getElementById('root');

// copy/cut go to the REAL system clipboard (that is the point — a scripted
// copy has to leave it where the key press would). Save the developer's
// clipboard and put it back at the end so running the suite doesn't eat it.
// __read/__write are the synchronous primitives behind the Promise-returning
// navigator.clipboard.readText/writeText; a sync test wants them directly.
const clipboardBefore = navigator.clipboard.__read();
function clipboard() { return navigator.clipboard.__read(); }

function press(key, mod) {
    keyDown(key, 0, mod || 0);
    keyUp(key, 0, mod || 0);
    flush();
}

function freshHost(html) {
    root.innerHTML =
        '<div id="ed" contenteditable="true" ' +
        'style="width:320px;height:60px;font-size:16px">' + (html || '') + '</div>';
    flush();
    return document.getElementById('ed');
}

function caret(ed, off, node) {
    const sel = window.getSelection();
    sel.collapse(node || ed.firstChild, off);
    flush();
}

function selectRange(ed, start, end, node) {
    const sel = window.getSelection();
    const n = node || ed.firstChild;
    sel.collapse(n, start);
    sel.extend(n, end);
    flush();
}

// ---------------------------------------------------------------------------
// queryCommandSupported: a static property of the name
// ---------------------------------------------------------------------------
{
    const supported = ['insertText', 'insertLineBreak', 'insertParagraph',
                       'delete', 'forwardDelete', 'undo', 'redo',
                       'selectAll', 'copy', 'cut', 'paste'];
    for (const cmd of supported) {
        assert(document.queryCommandSupported(cmd),
               cmd + ' should be supported');
    }

    // Not implemented: these need an inline-formatting model plaintext-v1
    // does not have. They must say so rather than no-op.
    for (const cmd of ['bold', 'italic', 'underline', 'foreColor',
                       'createLink', 'insertImage', 'formatBlock']) {
        assert(!document.queryCommandSupported(cmd),
               cmd + ' should report unsupported');
        assert(!document.execCommand(cmd),
               cmd + ' should return false');
    }

    // Unknown names are simply unsupported, not an error.
    assert(!document.queryCommandSupported('notACommand'), 'unknown unsupported');
    assert(!document.execCommand('notACommand'), 'unknown returns false');

    // Command names match case-insensitively, as they do in browsers.
    assert(document.queryCommandSupported('INSERTTEXT'), 'upper-case name');
    assert(document.queryCommandSupported('InsertText'), 'mixed-case name');
}

// ---------------------------------------------------------------------------
// insertText matches typing
// ---------------------------------------------------------------------------
{
    const ed = freshHost('hello');
    caret(ed, 5);
    assert(document.execCommand('insertText', false, ' world'),
           'insertText returns true');
    assert(ed.textContent === 'hello world',
           'insertText appended: ' + ed.textContent);

    // The caret must land after the insertion, so a second command continues
    // rather than overwriting.
    assert(document.execCommand('insertText', false, '!'), 'second insertText');
    assert(ed.textContent === 'hello world!',
           'caret followed the insertion: ' + ed.textContent);

    // Multi-byte text goes in whole — the offsets underneath are UTF-8 bytes.
    assert(document.execCommand('insertText', false, ' 日本😀'), 'utf8 insertText');
    assert(ed.textContent === 'hello world! 日本😀',
           'multi-byte insert: ' + ed.textContent);
}

// insertText over a selection replaces it, the same as typing over one.
{
    const ed = freshHost('abcdef');
    selectRange(ed, 1, 4);        // "bcd"
    document.execCommand('insertText', false, 'X');
    assert(ed.textContent === 'aXef', 'replaced selection: ' + ed.textContent);
}

// An empty insertText is a no-op that still reports success — there is
// nothing to insert, but the command did run.
{
    const ed = freshHost('abc');
    caret(ed, 3);
    let inputs = 0;
    ed.addEventListener('input', function () { inputs++; });
    assert(document.execCommand('insertText', false, ''), 'empty insertText ok');
    assert(ed.textContent === 'abc', 'unchanged');
    assert(inputs === 0, 'no input event for a zero-length edit');
}

// ---------------------------------------------------------------------------
// delete / forwardDelete match Backspace / Delete
// ---------------------------------------------------------------------------
{
    // Same start state, one via key and one via command: identical results.
    const viaKey = freshHost('a😀b');
    caret(viaKey, 3);             // after the emoji (UTF-16: a=1, emoji=2)
    press(SDLK_BACKSPACE);
    const keyResult = viaKey.textContent;

    const viaCmd = freshHost('a😀b');
    caret(viaCmd, 3);
    assert(document.execCommand('delete'), 'delete returns true');
    assert(viaCmd.textContent === keyResult,
           'delete matched Backspace: ' + viaCmd.textContent + ' vs ' + keyResult);
    assert(viaCmd.textContent === 'ab', 'whole emoji removed: ' + viaCmd.textContent);
}
{
    const viaKey = freshHost('a😀b');
    caret(viaKey, 1);
    press(SDLK_DELETE);
    const keyResult = viaKey.textContent;

    const viaCmd = freshHost('a😀b');
    caret(viaCmd, 1);
    assert(document.execCommand('forwardDelete'), 'forwardDelete returns true');
    assert(viaCmd.textContent === keyResult,
           'forwardDelete matched Delete: ' + viaCmd.textContent);
    assert(viaCmd.textContent === 'ab', 'forward-deleted emoji');
}

// delete with a range selected removes the range, not one character.
{
    const ed = freshHost('abcdef');
    selectRange(ed, 1, 4);
    document.execCommand('delete');
    assert(ed.textContent === 'aef', 'range deleted: ' + ed.textContent);
}

// ---------------------------------------------------------------------------
// insertLineBreak / insertParagraph match Enter
// ---------------------------------------------------------------------------
{
    const viaKey = freshHost('ab');
    caret(viaKey, 1);
    press(SDLK_RETURN);
    const keyHTML = viaKey.innerHTML;

    const viaCmd = freshHost('ab');
    caret(viaCmd, 1);
    assert(document.execCommand('insertLineBreak'), 'insertLineBreak true');
    assert(viaCmd.innerHTML === keyHTML,
           'insertLineBreak matched Enter: ' + viaCmd.innerHTML + ' vs ' + keyHTML);
    assert(viaCmd.querySelector('br') !== null, 'a <br> was inserted');

    // insertParagraph is a separate name that lands on the same edit while
    // contenteditable is plaintext-only — it must still be callable.
    const viaPara = freshHost('ab');
    caret(viaPara, 1);
    assert(document.execCommand('insertParagraph'), 'insertParagraph true');
    assert(viaPara.innerHTML === keyHTML,
           'insertParagraph matched Enter: ' + viaPara.innerHTML);
}

// ---------------------------------------------------------------------------
// undo / redo drive the same history Ctrl+Z does
// ---------------------------------------------------------------------------
{
    const ed = freshHost('start');
    caret(ed, 5);
    document.execCommand('insertText', false, 'ABC');
    assert(ed.textContent === 'startABC', 'edit landed');

    assert(document.execCommand('undo'), 'undo returns true');
    assert(ed.textContent === 'start', 'undo reverted: ' + ed.textContent);

    assert(document.execCommand('redo'), 'redo returns true');
    assert(ed.textContent === 'startABC', 'redo reapplied: ' + ed.textContent);

    // And a scripted edit is undoable by the keyboard: one shared history,
    // not one per entry point.
    press(SDLK_Z, KMOD_LCTRL);
    assert(ed.textContent === 'start', 'Ctrl+Z undid the scripted edit');
}

// undo with nothing to undo reports false rather than claiming success.
{
    const ed = freshHost('untouched');
    caret(ed, 0);
    assert(!document.execCommand('undo'), 'undo on empty history is false');
    assert(!document.queryCommandEnabled('undo'), 'undo not enabled');
    assert(ed.textContent === 'untouched', 'nothing changed');
}

// ---------------------------------------------------------------------------
// selectAll
// ---------------------------------------------------------------------------
{
    const ed = freshHost('some text');
    caret(ed, 2);
    assert(document.execCommand('selectAll'), 'selectAll returns true');
    const sel = window.getSelection();
    assert(sel.toString() === 'some text',
           'whole host selected: "' + sel.toString() + '"');

    // And the selection it leaves is a real one an edit consumes.
    document.execCommand('insertText', false, 'new');
    assert(ed.textContent === 'new', 'selectAll + insert replaced all');
}

// ---------------------------------------------------------------------------
// copy / cut / paste round-trip through the clipboard
// ---------------------------------------------------------------------------
{
    const ed = freshHost('copy me please');
    selectRange(ed, 0, 7);        // "copy me"
    assert(document.execCommand('copy'), 'copy returns true');
    assert(ed.textContent === 'copy me please', 'copy did not mutate');

    // The copy has to have reached the real clipboard, or a later paste
    // would see stale text.
    assert(clipboard() === 'copy me',
           'clipboard holds the copied text: ' + clipboard());

    // Paste it back at the end.
    caret(ed, ed.textContent.length, ed.firstChild);
    assert(document.execCommand('paste'), 'paste returns true');
    assert(ed.textContent === 'copy me pleasecopy me',
           'pasted at caret: ' + ed.textContent);
}
{
    const ed = freshHost('cut this out');
    selectRange(ed, 0, 4);        // "cut "
    assert(document.execCommand('cut'), 'cut returns true');
    assert(ed.textContent === 'this out', 'cut removed text: ' + ed.textContent);
    assert(clipboard() === 'cut ', 'clipboard holds the cut text');
}

// copy/cut with a collapsed selection have nothing to take.
{
    const ed = freshHost('abc');
    caret(ed, 1);
    assert(!document.queryCommandEnabled('copy'), 'copy disabled when collapsed');
    assert(!document.queryCommandEnabled('cut'), 'cut disabled when collapsed');
    assert(!document.execCommand('copy'), 'copy returns false when collapsed');
    assert(ed.textContent === 'abc', 'cut/copy left content alone');
}

// ---------------------------------------------------------------------------
// Commands outside an editable report false and change nothing
// ---------------------------------------------------------------------------
{
    root.innerHTML = '<div id="plain">not editable</div>';
    flush();
    const plain = document.getElementById('plain');
    const sel = window.getSelection();
    sel.collapse(plain.firstChild, 3);
    flush();

    for (const cmd of ['insertText', 'delete', 'forwardDelete',
                       'insertLineBreak', 'undo', 'redo']) {
        assert(!document.execCommand(cmd, false, 'X'),
               cmd + ' should be false outside an editable');
        assert(!document.queryCommandEnabled(cmd),
               cmd + ' should be disabled outside an editable');
    }
    assert(plain.textContent === 'not editable',
           'nothing was edited: ' + plain.textContent);

    // selectAll is the exception: it deliberately falls back to the body,
    // matching Ctrl+A over ordinary text.
    assert(document.execCommand('selectAll'), 'selectAll works outside editable');
    assert(window.getSelection().toString().indexOf('not editable') >= 0,
           'body selection includes the text');
}

// ---------------------------------------------------------------------------
// execCommand fires beforeinput/input, and beforeinput can cancel it
// ---------------------------------------------------------------------------
{
    const ed = freshHost('abc');
    caret(ed, 3);
    const seen = [];
    ed.addEventListener('beforeinput', function (e) { seen.push('before:' + e.inputType); });
    ed.addEventListener('input', function (e) { seen.push('input:' + e.inputType); });

    document.execCommand('insertText', false, 'X');
    assert(seen.join(',') === 'before:insertText,input:insertText',
           'events fired in order: ' + seen.join(','));
    assert(ed.textContent === 'abcX', 'edit applied');
}
{
    const ed = freshHost('abc');
    caret(ed, 3);
    ed.addEventListener('beforeinput', function (e) { e.preventDefault(); });
    document.execCommand('insertText', false, 'X');
    assert(ed.textContent === 'abc',
           'a canceled beforeinput blocked the edit: ' + ed.textContent);
}

// ---------------------------------------------------------------------------
// The optional arguments really are optional
// ---------------------------------------------------------------------------
{
    const ed = freshHost('abc');
    caret(ed, 3);
    // One argument, as most callers write it.
    assert(document.execCommand('selectAll'), 'one-arg call');
    caret(ed, 3);
    assert(document.execCommand('delete'), 'one-arg delete');
    assert(ed.textContent === 'ab', 'one-arg delete applied: ' + ed.textContent);
}

navigator.clipboard.__write(clipboardBefore);

console.log('PASS test_exec_command.js');
