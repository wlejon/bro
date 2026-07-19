// Undo GRANULARITY parity between <textarea> and contenteditable.
//
// Runs the SAME edit sequence against both surfaces and asserts that each
// Ctrl+Z reverts the same logical step on both. Two editing surfaces that
// undo at different granularity is the failure users actually notice, so
// this compares step COUNTS and per-step text, not implementation details.
//
// Comparison is on text content only: the DOM shape of a contenteditable
// host and the flat value of a textarea are legitimately different (a <br>
// vs a "\n"), so each surface reports its text through its own reader.

const SDLK_RETURN = 13;
const SDLK_BACKSPACE = 8;
const SDLK_DELETE = 127;
const SDLK_Z = 122;
const SDLK_LEFT = 0x40000050;
const KMOD_LCTRL = 0x0040;

const root = document.getElementById('root');

function press(key, mod) {
    keyDown(key, 0, mod || 0);
    keyUp(key, 0, mod || 0);
    flush();
}
function undo() { press(SDLK_Z, KMOD_LCTRL); }

// --- The two surfaces, behind one interface -------------------------------

function textareaSurface(initial) {
    root.innerHTML = '<textarea id="ta" style="width:300px;height:80px">' +
                     (initial || '') + '</textarea>';
    flush();
    const el = document.getElementById('ta');
    const r = el.getBoundingClientRect();
    click(r.left + 5, r.top + 8);
    flush();
    return {
        name: 'textarea',
        text: () => el.value,
        // Textarea offsets are byte offsets into the flat value.
        caret: (off) => { el.setSelectionRange(off, off); flush(); },
        select: (a, b) => { el.setSelectionRange(a, b); flush(); },
        sel: () => el.selectionStart + ':' + el.selectionEnd,
    };
}

function editableSurface(initial) {
    root.innerHTML = '<div id="ed" contenteditable="true" ' +
                     'style="width:300px;height:80px;font-size:16px">' +
                     (initial || '') + '</div>';
    flush();
    const el = document.getElementById('ed');
    const r = el.getBoundingClientRect();
    click(r.left + 5, r.top + 12);
    if (el.firstChild) window.getSelection().collapse(el.firstChild, 0);
    else window.getSelection().collapse(el, 0);
    flush();
    return {
        name: 'contenteditable',
        text: () => el.textContent,
        caret: (off) => {
            const s = window.getSelection();
            // Empty host: the caret is an element position, not a text offset.
            if (el.firstChild) s.collapse(el.firstChild, off);
            else s.collapse(el, 0);
            flush();
        },
        select: (a, b) => {
            const s = window.getSelection();
            s.setBaseAndExtent(el.firstChild, a, el.firstChild, b);
            flush();
        },
        sel: () => {
            const s = window.getSelection();
            return s.anchorOffset + ':' + s.focusOffset;
        },
    };
}

// Run `edits` on a surface, then undo until the history is spent, recording
// the text after every undo. The trace is the undo granularity.
function trace(makeSurface, initial, edits) {
    const s = makeSurface(initial);
    edits(s);
    const steps = [s.text()];
    // Cap the loop: a stuck stack must fail loudly rather than hang.
    for (let i = 0; i < 12; i++) {
        const before = s.text();
        undo();
        const after = s.text();
        if (after === before) break;
        steps.push(after);
    }
    return steps;
}

function parity(label, initial, edits) {
    const ta = trace(textareaSurface, initial, edits);
    const ce = trace(editableSurface, initial, edits);
    assert(ta.length === ce.length,
           label + ': undo step COUNT differs — textarea ' + ta.length +
           ' ' + JSON.stringify(ta) + ' vs contenteditable ' + ce.length +
           ' ' + JSON.stringify(ce));
    for (let i = 0; i < ta.length; i++) {
        assert(ta[i] === ce[i],
               label + ': undo step ' + i + ' differs — textarea ' +
               JSON.stringify(ta[i]) + ' vs contenteditable ' +
               JSON.stringify(ce[i]));
    }
    return ta;
}

// --- A typing run is one entry on both ------------------------------------
parity('typing run', '', (s) => {
    textInput('a'); textInput('b'); textInput('c'); flush();
});

// --- A caret move splits a typing run identically on both -----------------
parity('typing split by caret move', '', (s) => {
    textInput('a'); textInput('b'); flush();
    press(SDLK_LEFT);
    textInput('c'); flush();
});

// --- A backspace run is one entry; typing after it is another -------------
parity('backspace run then typing', 'hello', (s) => {
    s.caret(5);
    press(SDLK_BACKSPACE); press(SDLK_BACKSPACE);
    textInput('p'); flush();
});

// --- A forward-delete run is one entry, distinct from a backspace run -----
parity('delete run', 'hello', (s) => {
    s.caret(0);
    press(SDLK_DELETE); press(SDLK_DELETE);
});

// --- Backspaces and deletes do not merge with each other ------------------
parity('backspace then delete', 'abcdef', (s) => {
    s.caret(3);
    press(SDLK_BACKSPACE);
    press(SDLK_DELETE);
});

// --- Typing over a selection is discrete, then a fresh typing run ---------
parity('replace selection then type', 'hello world', (s) => {
    s.select(0, 5);
    textInput('X'); flush();
    textInput('Y'); textInput('Z'); flush();
});

// --- Paste stands alone between two typing runs ---------------------------
parity('typing, paste, typing', '', (s) => {
    textInput('a'); flush();
    paste('MID');
    textInput('b'); flush();
});

// --- Enter stands alone between two typing runs ---------------------------
// The textarea stores a newline; the contenteditable host stores a <br>.
// Both are the same line break to a user, so the CE side is read with a
// reader that renders <br> as a newline — otherwise undoing the break would
// look like "no change" and hide a granularity difference rather than
// expose one.
{
    function ceLineText(el) {
        let out = '';
        for (const n of el.childNodes) {
            if (n.nodeType === 3) out += n.data;
            else if (n.nodeName === 'BR') out += String.fromCharCode(10);
            else out += n.textContent;
        }
        return out;
    }
    function enterTrace(makeSurface, read) {
        const s = makeSurface('');
        textInput('a'); flush();
        press(SDLK_RETURN);
        textInput('b'); flush();
        const steps = [read(s)];
        for (let i = 0; i < 12; i++) {
            undo();
            const t = read(s);
            if (t === steps[steps.length - 1]) break;
            steps.push(t);
        }
        return steps;
    }
    const ta = enterTrace(textareaSurface, (s) => s.text());
    const ce = enterTrace(editableSurface,
                          () => ceLineText(document.getElementById('ed')));
    assert(ta.length === ce.length,
           'Enter: undo step count differs — textarea ' + JSON.stringify(ta) +
           ' vs contenteditable ' + JSON.stringify(ce));
    for (let i = 0; i < ta.length; i++) {
        assert(ta[i] === ce[i],
               'Enter: undo step ' + i + ' differs — ' + JSON.stringify(ta[i]) +
               ' vs ' + JSON.stringify(ce[i]));
    }
}

// --- An IME commit is one entry on both surfaces --------------------------
{
    const taSteps = trace(textareaSurface, 'hi ', (s) => {
        s.caret(3);
        imeCompose('ni', 2); imeCompose('nih', 3); imeCommit('你好'); flush();
    });
    const ceSteps = trace(editableSurface, 'hi ', (s) => {
        s.caret(3);
        imeCompose('ni', 2); imeCompose('nih', 3); imeCommit('你好'); flush();
    });
    assert(taSteps.length === ceSteps.length,
           'IME commit: step count differs — ' + JSON.stringify(taSteps) +
           ' vs ' + JSON.stringify(ceSteps));
    for (let i = 0; i < taSteps.length; i++) {
        assert(taSteps[i] === ceSteps[i],
               'IME commit: step ' + i + ' differs — ' +
               JSON.stringify(taSteps[i]) + ' vs ' + JSON.stringify(ceSteps[i]));
    }
}

// --- A canceled composition resurrects the replaced selection on both -----
{
    function cancelTrace(makeSurface) {
        const s = makeSurface('hello world');
        s.select(0, 5);
        imeCompose('ab', 2); flush();
        const during = s.text();
        imeCancel(); flush();
        return [during, s.text(), s.sel()];
    }
    const ta = cancelTrace(textareaSurface);
    const ce = cancelTrace(editableSurface);
    assert(ta[0] === ce[0], 'IME cancel: text during composition differs — ' +
           JSON.stringify(ta[0]) + ' vs ' + JSON.stringify(ce[0]));
    assert(ta[1] === 'hello world' && ce[1] === 'hello world',
           'IME cancel resurrects the replaced selection on both — textarea ' +
           JSON.stringify(ta[1]) + ', contenteditable ' + JSON.stringify(ce[1]));
    assert(ta[2] === ce[2], 'IME cancel: restored selection differs — ' +
           ta[2] + ' vs ' + ce[2]);
}

console.log('textarea / contenteditable undo parity: OK');
