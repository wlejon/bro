// Contenteditable text editing: caret stepping and Backspace/Delete operate on
// whole characters, not on bytes.
//
// The engine stores text node data as UTF-8 and Selection offsets as byte
// offsets into that data; the JS binding converts to and from UTF-16 code units
// at the boundary. Stepping a raw ±1 through a byte offset therefore lands
// inside a multi-byte character, and deleting one byte leaves invalid UTF-8
// behind — the corruption these tests exist to catch.
//
// Deletion steps one *code point*, matching <input>/<textarea> (which call
// utf8Prev/utf8Next from the same key handler). Caret arrow movement in the
// controls steps by shaper cluster because caret geometry has to agree with
// the glyph that is drawn; deletion has no such constraint and stays on the
// simpler, font-independent domain.

const SDLK_BACKSPACE = 8;
const SDLK_DELETE = 127;
const SDLK_LEFT = 0x40000050;
const SDLK_RIGHT = 0x4000004F;
const SDLK_Z = 122;
const SDLK_Y = 121;
const KMOD_LCTRL = 0x0040;

const root = document.getElementById('root');

function press(key, mod) {
    keyDown(key, 0, mod || 0);
    keyUp(key, 0, mod || 0);
    flush();
}
function undo() { press(SDLK_Z, KMOD_LCTRL); }
function redo() { press(SDLK_Y, KMOD_LCTRL); }

function freshHost(html) {
    root.innerHTML =
        '<div id="ed" contenteditable="true" ' +
        'style="width:320px;height:60px;font-size:16px">' + (html || '') + '</div>';
    flush();
    return document.getElementById('ed');
}

// Caret at UTF-16 offset `off` of `node` (the host's first text node by
// default). The binding converts to the engine's byte offset.
function caret(ed, off, node) {
    const sel = window.getSelection();
    sel.collapse(node || ed.firstChild, off);
    flush();
}

// ---------------------------------------------------------------------------
// Backspace removes one whole character, whatever its UTF-8 length
// ---------------------------------------------------------------------------
{
    // [text, expected after one Backspace at the end]
    const cases = [
        ['aé', 'ae'],        // combining acute: 2 bytes, its own code point
        ['aé', 'a'],          // precomposed e-acute: 2 bytes
        ['a日', 'a'],          // CJK: 3 bytes
        ['a😀', 'a'],    // emoji U+1F600: 4 bytes / 1 code point
    ];
    for (const [text, want] of cases) {
        const ed = freshHost(text);
        caret(ed, ed.firstChild.data.length);
        press(SDLK_BACKSPACE);
        assert(ed.textContent === want,
               'backspace in ' + JSON.stringify(text) + ' gave ' +
               JSON.stringify(ed.textContent) + ', want ' + JSON.stringify(want));
    }
}

// ---------------------------------------------------------------------------
// Delete removes one whole character forward
// ---------------------------------------------------------------------------
{
    const cases = [
        ['́b', 'b'],
        ['éb', 'b'],
        ['日b', 'b'],
        ['😀b', 'b'],
    ];
    for (const [text, want] of cases) {
        const ed = freshHost(text);
        caret(ed, 0);
        press(SDLK_DELETE);
        assert(ed.textContent === want,
               'delete in ' + JSON.stringify(text) + ' gave ' +
               JSON.stringify(ed.textContent) + ', want ' + JSON.stringify(want));
    }
}

// ---------------------------------------------------------------------------
// Deleting every character of a multi-byte string empties it, and each step
// leaves the text node holding well-formed UTF-8 (a byte-wise delete would
// surface U+FFFD replacement characters on the way down)
// ---------------------------------------------------------------------------
{
    const text = 'aé日😀z';
    const ed = freshHost(text);
    caret(ed, ed.firstChild.data.length);
    for (let i = 0; i < 5; i++) {
        press(SDLK_BACKSPACE);
        assert(ed.textContent.indexOf('�') === -1,
               'step ' + i + ' left a replacement char: ' +
               JSON.stringify(ed.textContent));
    }
    assert(ed.textContent === '',
           'five backspaces empty five characters, got ' +
           JSON.stringify(ed.textContent));
}

// ---------------------------------------------------------------------------
// Undo/redo round-trips multi-byte deletion byte-identically
// ---------------------------------------------------------------------------
{
    const text = 'aé日😀z';
    const ed = freshHost('');
    caret(ed, 0, ed);
    for (const ch of Array.from(text)) textInput(ch);
    flush();
    assert(ed.textContent === text,
           'typed the multi-byte string, got ' + JSON.stringify(ed.textContent));

    press(SDLK_BACKSPACE);
    press(SDLK_BACKSPACE);
    assert(ed.textContent === 'aé日',
           'two backspaces drop emoji then z, got ' +
           JSON.stringify(ed.textContent));

    undo();
    assert(ed.textContent === text,
           'undo restores the string byte-identically, got ' +
           JSON.stringify(ed.textContent));

    redo();
    assert(ed.textContent === 'aé日',
           'redo re-applies the deletion, got ' + JSON.stringify(ed.textContent));

    undo();
    assert(ed.textContent === text, 'undo again after redo');
}

// ---------------------------------------------------------------------------
// Arrow keys step whole characters too — a byte step lands mid-character,
// where the UTF-16 conversion snaps back and the caret appears stuck
// ---------------------------------------------------------------------------
{
    const ed = freshHost('a日b');
    const sel = window.getSelection();
    caret(ed, 0);
    press(SDLK_RIGHT);
    assert(sel.focusOffset === 1, 'right past "a" -> 1, got ' + sel.focusOffset);
    press(SDLK_RIGHT);
    assert(sel.focusOffset === 2,
           'right past the CJK char -> 2, got ' + sel.focusOffset);
    press(SDLK_RIGHT);
    assert(sel.focusOffset === 3, 'right past "b" -> 3, got ' + sel.focusOffset);
    press(SDLK_LEFT);
    assert(sel.focusOffset === 2, 'left back over "b" -> 2, got ' + sel.focusOffset);
    press(SDLK_LEFT);
    assert(sel.focusOffset === 1,
           'left back over the CJK char -> 1, got ' + sel.focusOffset);
}

// A surrogate pair is one code point: one arrow step crosses both UTF-16 units.
{
    const ed = freshHost('a😀b');
    const sel = window.getSelection();
    caret(ed, 0);
    press(SDLK_RIGHT);
    press(SDLK_RIGHT);
    assert(sel.focusOffset === 3,
           'one step crosses the whole surrogate pair -> 3, got ' + sel.focusOffset);
}

console.log('contenteditable editing tests passed');
