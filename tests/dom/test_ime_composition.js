// IME composition (imeCompose / imeCommit / imeCancel — the headless seam for
// SDL_EVENT_TEXT_EDITING / TEXT_INPUT). Browser-matching semantics:
//   - the preedit is visible in .value during composition (provisional text)
//   - compositionstart/update/end and input(insertCompositionText) fire in
//     Chrome's observable order
//   - the whole committed composition is ONE undo entry; cancel leaves none
//   - composing over a selection replaces it (same single entry)
//   - caret moves / blur / mouse presses mid-composition COMMIT the preedit
// Offsets are UTF-8 byte offsets (existing selectionStart/End convention).

const SDLK_LEFT = 0x40000050;
const SDLK_Z = 122;
const SDLK_Y = 121;
const KMOD_LCTRL = 0x0040;

const root = document.getElementById('root');

function press(key, mod) {
    keyDown(key, 0, mod || 0);
    keyUp(key, 0, mod || 0);
}
function undo() { press(SDLK_Z, KMOD_LCTRL); }
function redo() { press(SDLK_Y, KMOD_LCTRL); }

function freshInput(id) {
    root.innerHTML = '<input id="' + id + '" type="text" style="width:260px">';
    flush();
    const el = document.getElementById(id);
    const r = el.getBoundingClientRect();
    click(r.left + 5, r.top + r.height / 2);
    return el;
}

// --- Full compose → commit round-trip with event-order recording -----------
{
    const el = freshInput('c1');
    const seq = [];
    el.addEventListener('compositionstart', e => seq.push('cs:' + e.data));
    el.addEventListener('compositionupdate', e => seq.push('cu:' + e.data));
    el.addEventListener('compositionend', e => seq.push('ce:' + e.data));
    el.addEventListener('input', e =>
        seq.push('in:' + e.inputType + ':' + (e.data === null ? '' : e.data) +
                 ':' + (e.isComposing ? 1 : 0)));

    imeCompose('に');                  // に
    assert(el.value === 'に', 'preedit visible in value, got: ' + JSON.stringify(el.value));
    assert(el.selectionStart === 3 && el.selectionEnd === 3,
           'composition caret at preedit end (3 bytes), got: ' + el.selectionStart);

    imeCompose('にほ');            // にほ
    assert(el.value === 'にほ', 'updated preedit replaces the old one, got: ' + JSON.stringify(el.value));
    assert(el.selectionStart === 6, 'caret tracks the growing preedit, got: ' + el.selectionStart);

    imeCommit('日本');             // 日本
    assert(el.value === '日本', 'commit replaces preedit with committed text, got: ' + JSON.stringify(el.value));
    assert(el.selectionStart === 6 && el.selectionEnd === 6,
           'caret after commit at byte end of committed text, got: ' + el.selectionStart);

    const expected = [
        'cs:',                                        // no selection replaced
        'cu:に', 'in:insertCompositionText:に:1',
        'cu:にほ', 'in:insertCompositionText:にほ:1',
        'cu:日本', 'in:insertCompositionText:日本:1',
        'ce:日本',
    ].join(',');
    assert(seq.join(',') === expected,
           'event order start→update/input pairs→final update/input→end, got: ' + seq.join(','));

    // ONE undo entry for the whole composition.
    undo();
    assert(el.value === '', 'one undo removes the whole committed composition, got: ' + JSON.stringify(el.value));
    assert(el.selectionStart === 0, 'undo restores pre-composition caret, got: ' + el.selectionStart);
    redo();
    assert(el.value === '日本', 'redo restores the commit, got: ' + JSON.stringify(el.value));
    assert(el.selectionStart === 6, 'redo restores post-commit caret, got: ' + el.selectionStart);
}

// --- Cursor position within the preedit (codepoints → bytes) ---------------
{
    const el = freshInput('c2');
    imeCompose('にほん', 1);   // にほん, cursor after codepoint 1
    assert(el.selectionStart === 3,
           'composition cursor 1 cp = 3 bytes, got: ' + el.selectionStart);
    imeCancel();
}

// --- Cancel restores the value and leaves no undo entry ---------------------
{
    const el = freshInput('c3');
    textInput('ab');
    let ended = '';
    el.addEventListener('compositionend', e => { ended = 'ce:' + e.data; });
    imeCompose('か');                  // か
    assert(el.value === 'abか', 'preedit appended at caret, got: ' + JSON.stringify(el.value));
    imeCancel();
    assert(el.value === 'ab', 'cancel restores the pre-composition value, got: ' + JSON.stringify(el.value));
    assert(el.selectionStart === 2, 'cancel restores the caret, got: ' + el.selectionStart);
    assert(ended === 'ce:', 'compositionend data is "" on cancel, got: ' + ended);

    undo();
    assert(el.value === '', 'cancel left no undo entry — undo removes the typing, got: ' + JSON.stringify(el.value));
    redo();
    assert(el.value === 'ab', 'redo restores only the typing, got: ' + JSON.stringify(el.value));
    undo();
    undo();
    assert(el.value === '', 'no further entries beyond the typing run');
}

// --- Composing over a selection: replaced, single undo entry ----------------
{
    const el = freshInput('c4');
    textInput('hello world');
    el.setSelectionRange(6, 11);           // "world"
    let csData = null;
    el.addEventListener('compositionstart', e => { csData = e.data; });

    imeCompose('せ');                  // せ
    assert(csData === 'world', 'compositionstart.data is the replaced selection, got: ' + csData);
    assert(el.value === 'hello せ', 'selection replaced by preedit, got: ' + JSON.stringify(el.value));

    imeCommit('世界');             // 世界
    assert(el.value === 'hello 世界', 'committed over the selection, got: ' + JSON.stringify(el.value));
    assert(el.selectionStart === 12, 'caret after committed bytes (6+6), got: ' + el.selectionStart);

    undo();
    assert(el.value === 'hello world', 'single undo restores replaced text, got: ' + JSON.stringify(el.value));
    assert(el.selectionStart === 6 && el.selectionEnd === 11,
           'undo restores the selection 6..11, got: ' + el.selectionStart + '..' + el.selectionEnd);
    redo();
    assert(el.value === 'hello 世界', 'redo re-applies the commit, got: ' + JSON.stringify(el.value));
}

// --- Blur mid-composition commits the current preedit -----------------------
{
    const el = freshInput('c5');
    const seq = [];
    el.addEventListener('compositionend', e => seq.push('ce:' + e.data));
    imeCompose('にほ');
    el.blur();
    flush();
    assert(el.value === 'にほ', 'blur commits the preedit as final text, got: ' + JSON.stringify(el.value));
    assert(seq.join(',') === 'ce:にほ', 'compositionend fired on blur-commit, got: ' + seq.join(','));
    undo();
    assert(el.value === 'にほ', 'undo needs focus — value unchanged while blurred');
}

// --- Click elsewhere mid-composition commits --------------------------------
{
    root.innerHTML = '<input id="c6a" type="text" style="width:120px">' +
                     '<input id="c6b" type="text" style="width:120px">';
    flush();
    const a = document.getElementById('c6a');
    const b = document.getElementById('c6b');
    const ra = a.getBoundingClientRect();
    click(ra.left + 5, ra.top + ra.height / 2);
    let ended = false;
    a.addEventListener('compositionend', () => { ended = true; });
    imeCompose('き');                  // き
    const rb = b.getBoundingClientRect();
    click(rb.left + 5, rb.top + rb.height / 2);
    assert(a.value === 'き', 'mouse press elsewhere commits the preedit, got: ' + JSON.stringify(a.value));
    assert(ended, 'compositionend fired on the click-away commit');
}

// --- Stray caret key mid-composition commits --------------------------------
{
    const el = freshInput('c7');
    imeCompose('あ');                  // あ
    press(SDLK_LEFT);
    assert(el.value === 'あ', 'arrow key commits the preedit, got: ' + JSON.stringify(el.value));
    assert(el.selectionStart === 0, 'then the arrow moves the caret (3 → 0), got: ' + el.selectionStart);
    // Committed = one undo entry.
    undo();
    assert(el.value === '', 'the committed preedit is one undo entry, got: ' + JSON.stringify(el.value));
}

// --- Typing run + composition + typing run = three undo entries -------------
{
    const el = freshInput('c8');
    textInput('a'); textInput('b');
    imeCompose('ぬ');                  // ぬ
    imeCommit('日');                   // 日
    textInput('c'); textInput('d');
    assert(el.value === 'ab日cd', 'mixed runs typed, got: ' + JSON.stringify(el.value));

    undo();
    assert(el.value === 'ab日', 'undo 1 removes the trailing typing run, got: ' + JSON.stringify(el.value));
    undo();
    assert(el.value === 'ab', 'undo 2 removes the composition commit, got: ' + JSON.stringify(el.value));
    undo();
    assert(el.value === '', 'undo 3 removes the leading typing run, got: ' + JSON.stringify(el.value));
    redo(); redo(); redo();
    assert(el.value === 'ab日cd', 'three redos rebuild everything, got: ' + JSON.stringify(el.value));
}

// --- Textarea: composition in a line after an existing newline --------------
{
    root.innerHTML = '<textarea id="t1" rows="4" cols="24"></textarea>';
    flush();
    const ta = document.getElementById('t1');
    const r = ta.getBoundingClientRect();
    click(r.left + 5, r.top + 5);
    textInput('ab');
    press(13 /* SDLK_RETURN */);
    textInput('cd');
    ta.setSelectionRange(3, 3);            // start of "cd", after the newline
    imeCompose('ほ');                  // ほ
    assert(ta.value === 'ab\nほcd', 'preedit inserted mid-textarea, got: ' + JSON.stringify(ta.value));
    imeCompose('ほげ');            // ほげ
    imeCommit('ほげ');
    assert(ta.value === 'ab\nほげcd',
           'committed across the newline, got: ' + JSON.stringify(ta.value));
    assert(ta.selectionStart === 9, 'caret after committed bytes (3+6), got: ' + ta.selectionStart);

    undo();
    assert(ta.value === 'ab\ncd', 'one undo removes the textarea composition, got: ' + JSON.stringify(ta.value));
    assert(ta.selectionStart === 3 && ta.selectionEnd === 3,
           'undo restores the pre-composition caret, got: ' + ta.selectionStart);
}

// --- UTF-8 integrity: byte lengths and no split code points -----------------
{
    const el = freshInput('c9');
    imeCompose('に');
    imeCommit('日本語');       // 日本語 = 9 bytes
    assert(el.value === '日本語', 'exact committed string, got: ' + JSON.stringify(el.value));
    assert(el.selectionStart === 9, '3 CJK chars = 9 bytes, got: ' + el.selectionStart);
    // Backspace deletes one whole character (3 bytes), never a split byte.
    press(8 /* backspace */);
    assert(el.value === '日本', 'backspace removes one whole character, got: ' + JSON.stringify(el.value));
    assert(el.selectionStart === 6, 'caret back one character (3 bytes), got: ' + el.selectionStart);
}

// --- Programmatic .focus() enables composition (and typing) -----------------
{
    root.innerHTML = '<input id="c10" type="text">';
    flush();
    const el = document.getElementById('c10');
    el.focus();
    flush();
    imeCompose('は');                  // は
    imeCommit('波');                   // 波
    assert(el.value === '波', 'composition works after programmatic .focus(), got: ' + JSON.stringify(el.value));
    textInput('!');
    assert(el.value === '波!', 'plain typing works after .focus() too, got: ' + JSON.stringify(el.value));
}

// --- imeCommit without a composition behaves like textInput -----------------
{
    const el = freshInput('c11');
    imeCommit('あ');
    assert(el.value === 'あ', 'bare commit inserts like textInput, got: ' + JSON.stringify(el.value));
}

// --- Programmatic .value= mid-composition drops the stale composition -------
{
    const el = freshInput('c12');
    imeCompose('に');
    el.value = 'script';
    flush();
    imeCommit('X');
    // The stale composition was dropped with the history; the commit falls
    // through to a plain insert at the caret (still at byte 3 from the
    // preedit — a script value write does not move the control caret).
    assert(el.value === 'scrXipt',
           'script value kept, commit degraded to plain insert, got: ' + JSON.stringify(el.value));
}
