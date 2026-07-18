// selectionStart/selectionEnd/setSelectionRange must speak UTF-16 code units
// (the JS string domain), not the control's internal UTF-8 byte offsets.
// Chrome reference: `value.slice(selectionStart, selectionEnd)` is always the
// selected text; astral characters (emoji) count as TWO units.
//
// bro divergence (documented, byte-domain limitation): a UTF-16 index that
// lands BETWEEN the two units of a surrogate pair maps to the preceding
// character boundary — the byte domain cannot name the middle of a code point.

const root = document.getElementById('root');

const SDLK_BACKSPACE = 0x08;
const SDLK_Z = 122;
const KMOD_LCTRL = 0x0040;

root.innerHTML = '<input id="t" type="text"><textarea id="ta"></textarea>';
flush();
const t = document.getElementById('t');
const ta = document.getElementById('ta');

// Focus the input so the typing pipeline works later.
const r = t.getBoundingClientRect();
click(r.left + 5, r.top + 5);
assert(document.activeElement === t, 'input focused');

function setSel(el, v, s, e) {
    el.value = v;
    el.setSelectionRange(s, e);
}

// ---- ASCII unchanged ------------------------------------------------------
setSel(t, 'hello', 1, 4);
assert(t.selectionStart === 1 && t.selectionEnd === 4,
       'ascii range (1,4), got (' + t.selectionStart + ',' + t.selectionEnd + ')');
assert(t.value.slice(t.selectionStart, t.selectionEnd) === 'ell', 'ascii slice');

// Clamping beyond length
setSel(t, 'hello', 2, 99);
assert(t.selectionStart === 2 && t.selectionEnd === 5,
       'ascii clamp end to length, got (' + t.selectionStart + ',' + t.selectionEnd + ')');

// ---- 2-byte UTF-8 (é) -----------------------------------------------------
setSel(t, 'héllo', 0, 5);              // "héllo" — 5 UTF-16 units, 6 bytes
assert(t.value.length === 5, 'héllo length 5');
assert(t.selectionStart === 0 && t.selectionEnd === 5,
       'héllo full select (0,5), got (' + t.selectionStart + ',' + t.selectionEnd + ')');
assert(t.value.slice(t.selectionStart, t.selectionEnd) === 'héllo', 'héllo slice');

setSel(t, 'héllo', 2, 2);              // caret after é
assert(t.selectionStart === 2 && t.selectionEnd === 2, 'héllo caret at 2');

// ---- 3-byte UTF-8 (日本語) -------------------------------------------------
setSel(t, '日本語', 0, 3);      // 3 units, 9 bytes
assert(t.selectionStart === 0 && t.selectionEnd === 3,
       'nihongo full select (0,3), got (' + t.selectionStart + ',' + t.selectionEnd + ')');
setSel(t, '日本語', 1, 2);
assert(t.selectionStart === 1 && t.selectionEnd === 2, 'nihongo (1,2)');
assert(t.value.slice(t.selectionStart, t.selectionEnd) === '本', 'nihongo slice = 本');

// ---- astral (😀 = 2 UTF-16 units, 4 UTF-8 bytes) --------------------------
const astral = 'a\u{1F600}b';               // 4 units, 6 bytes
setSel(t, astral, 0, astral.length);
assert(t.value.length === 4, 'astral length 4');
assert(t.selectionStart === 0 && t.selectionEnd === 4,
       'astral full select (0,4), got (' + t.selectionStart + ',' + t.selectionEnd + ')');
assert(t.value.slice(t.selectionStart, t.selectionEnd) === astral, 'astral full slice');

setSel(t, astral, 1, 3);                    // exactly the emoji
assert(t.selectionStart === 1 && t.selectionEnd === 3,
       'astral emoji select (1,3), got (' + t.selectionStart + ',' + t.selectionEnd + ')');
assert(t.value.slice(t.selectionStart, t.selectionEnd) === '\u{1F600}', 'astral slice = emoji');

// Mid-surrogate index snaps to the preceding character boundary.
setSel(t, astral, 2, 3);                    // 2 = middle of the pair
assert(t.selectionStart === 1 && t.selectionEnd === 3,
       'mid-astral start snaps back to 1, got (' + t.selectionStart + ',' + t.selectionEnd + ')');
setSel(t, astral, 0, 2);                    // end mid-pair
assert(t.selectionStart === 0 && t.selectionEnd === 1,
       'mid-astral end snaps back to 1, got (' + t.selectionStart + ',' + t.selectionEnd + ')');

// Individual setters, same domain.
setSel(t, astral, 0, 0);
t.selectionEnd = 3;
assert(t.selectionStart === 0 && t.selectionEnd === 3, 'selectionEnd setter utf16');
t.selectionStart = 3;
assert(t.selectionStart === 3 && t.selectionEnd === 3, 'selectionStart setter utf16');
t.selectionStart = 99;                      // clamps to length in UTF-16 domain
assert(t.selectionStart === 4, 'selectionStart clamps to 4, got ' + t.selectionStart);

// ---- textarea speaks the same domain --------------------------------------
ta.value = '日本語 and \u{1F600}';   // 3 + 5 + 2 = 10 units
ta.setSelectionRange(0, ta.value.length);
assert(ta.selectionStart === 0 && ta.selectionEnd === ta.value.length,
       'textarea full select in utf16, got (' + ta.selectionStart + ',' + ta.selectionEnd + ')');
ta.setSelectionRange(1, 2);
assert(ta.value.slice(ta.selectionStart, ta.selectionEnd) === '本', 'textarea slice = 本');

// ---- typing / backspace / undo interplay at non-ASCII boundaries ----------
// The undo system is byte-internal; verify nothing JS-visible drifts.
t.value = '';
flush();
textInput('hé');
assert(t.value === 'hé', 'typed hé');
assert(t.selectionStart === 2 && t.selectionEnd === 2,
       'caret after hé = 2 units, got ' + t.selectionStart);

textInput('\u{1F600}');
assert(t.value === 'hé\u{1F600}', 'typed hé😀');
assert(t.selectionStart === 4,
       'caret after emoji = 4 units, got ' + t.selectionStart);

keyDown(SDLK_BACKSPACE); keyUp(SDLK_BACKSPACE);
assert(t.value === 'hé', 'backspace removes whole emoji, got: ' + t.value);
assert(t.selectionStart === 2, 'caret back to 2 units, got ' + t.selectionStart);

keyDown(SDLK_BACKSPACE); keyUp(SDLK_BACKSPACE);
assert(t.value === 'h', 'backspace removes é whole');
assert(t.selectionStart === 1, 'caret at 1, got ' + t.selectionStart);

// Undo (ctrl+z): the two backspaces coalesce into one delete entry, so undo
// restores both characters; caret must stay coherent in UTF-16.
keyDown(SDLK_Z, 0, KMOD_LCTRL); keyUp(SDLK_Z, 0, KMOD_LCTRL);
assert(t.value === 'hé\u{1F600}', 'undo restores deleted run, got: ' + t.value);
assert(t.selectionStart === 4, 'caret after undo = 4 units, got ' + t.selectionStart);

console.log('test_selection_utf16 OK');
