// Typing a space in a contenteditable moves the caret, and typing N spaces
// shows N spaces.
//
// Under `white-space: normal` a run of spaces renders as one, and a space at
// the start or end of a line renders as nothing. Correct CSS, and unusable as
// editing behaviour: the caret sat still while the DOM string grew, so the box
// looked like it was ignoring the spacebar until a visible character arrived —
// and then only one space appeared however many had been typed.
//
// The fix is what every browser does: store U+00A0 in the positions that would
// otherwise collapse. The expectations below are the exact encodings Chromium
// produces for the same keystrokes, checked against it case by case. There is
// more than one encoding that renders correctly; matching Chromium's keeps
// copy/paste and innerHTML round-trips comparable rather than merely adequate.

const root = document.getElementById('root');
const NB = ' ';

function host(initial, style) {
    root.innerHTML = '<div id="h" contenteditable="true" style="width:600px;' +
                     'font-family:Arial;font-size:20px;' + (style || '') + '">' +
                     initial + '</div>';
    flush();
    return document.getElementById('h');
}

// Put the caret at a byte offset in the host's text, then type.
function typeAt(h, offset, text) {
    const sel = window.getSelection();
    if (h.firstChild) {
        const r = document.createRange();
        r.setStart(h.firstChild, offset);
        r.collapse(true);
        sel.removeAllRanges();
        sel.addRange(r);
    } else {
        sel.collapse(h, 0);
    }
    for (const ch of text) {
        textInput(ch);
        flush();
    }
}

// Readable rendering of what got stored: `_` plain space, `+` no-break.
function shape(s) {
    return [...s].map((c) => (c === NB ? '+' : c === ' ' ? '_' : c)).join('');
}

// --- the encodings, against Chromium ----------------------------------------
{
    const cases = [
        // initial, caret, typed,  expected shape
        ['ab', 2, ' ',    'ab+',    'a lone space at the end cannot be plain'],
        ['ab', 2, ' c',   'ab_c',   'between two words a plain space renders'],
        ['ab', 2, '  c',  'ab+_c',  'alternating, so no two plain spaces meet'],
        ['ab', 2, '   c', 'ab+_+c', 'and it keeps alternating'],
        ['ab', 2, '   ',  'ab+_+',  'a run at the end still ends no-break'],
        ['ab', 2, '  ',   'ab++',   'alternating would end plain here, so it is forced'],
        ['ab', 0, ' ',    '+ab',    'a run at the start cannot be plain either'],
    ];
    for (const [initial, at, typed, want, why] of cases) {
        const h = host(initial);
        typeAt(h, at, typed);
        const got = shape(h.textContent);
        assert(got === want,
               `${JSON.stringify(initial)} caret ${at}, typed ${JSON.stringify(typed)}: ` +
               `${why} — want ${want}, got ${got}`);
    }
}

// --- a space in the middle of a word stays a plain space ---------------------
//
// The common case, and the one that must NOT be rewritten: rewriting every
// space to U+00A0 would fix the caret and silently break line wrapping, since
// a no-break space is not a wrap opportunity.
{
    const h = host('abcd');
    typeAt(h, 2, ' ');
    assert(shape(h.textContent) === 'ab_cd',
           'a space between two characters is plain, got ' + shape(h.textContent));
    const h2 = host('abcd');
    typeAt(h2, 2, '  ');
    assert(shape(h2.textContent) === 'ab+_cd',
           'and two of them alternate, got ' + shape(h2.textContent));
}

// --- the caret actually moves ------------------------------------------------
//
// The symptom the user sees. Asserted as geometry, because "the offset
// advanced" was already true while the caret stood still.
{
    const h = host('ab');
    const t = h.firstChild;

    function caretX(offset) {
        const r = document.createRange();
        r.setStart(h.firstChild, offset);
        r.collapse(true);
        return r.getBoundingClientRect().x;
    }

    const before = caretX(2);
    typeAt(h, 2, ' ');
    const after = caretX(3);
    assert(after > before + 1,
           `the caret advances past a typed space: ${before} -> ${after}`);

    typeAt(h, 3, ' ');
    const after2 = caretX(4);
    assert(after2 > after + 1,
           `and past a second one: ${after} -> ${after2}`);
}

// --- N spaces occupy N spaces of width --------------------------------------
//
// Independent of the encoding: whatever is stored, three typed spaces must be
// three times as wide as one.
{
    const width = (typed) => {
        const h = host('ab');
        typeAt(h, 2, typed + 'c');
        const r = document.createRange();
        r.setStart(h.firstChild, 0);
        r.setEnd(h.firstChild, h.firstChild.data.length);
        return r.getBoundingClientRect().width;
    };
    const w0 = width('');
    const w1 = width(' ');
    const w3 = width('   ');
    const one = w1 - w0;
    assert(one > 1, 'one typed space adds real width, got ' + one);
    assert(Math.abs((w3 - w0) - 3 * one) < 0.5,
           `three typed spaces are three times as wide as one: ` +
           `${(w3 - w0).toFixed(2)} vs ${(3 * one).toFixed(2)}`);
}

// --- pre-formatted text is left alone ----------------------------------------
//
// Under `white-space: pre` a plain space already renders, so there is nothing
// to rebalance and rewriting one to U+00A0 would change what the document
// means — a pre block is exactly where the two characters differ.
{
    const h = host('ab', 'white-space:pre;');
    typeAt(h, 2, '  ');
    assert(shape(h.textContent) === 'ab__',
           'spaces typed into a pre block stay plain, got ' + shape(h.textContent));
}

root.innerHTML = '';
