// CharacterData offsets must speak UTF-16 code units (the JS string domain),
// not bro's internal UTF-8 byte offsets. The oracle throughout is real JS
// string semantics: a Text node holding S must report the same .length as S,
// and substringData(i, n) must equal S.substr(i, n).
//
// Divergence classes exercised:
//   ASCII        1 byte  = 1 UTF-16 unit  (domains agree)
//   Latin-1 acc. 2 bytes = 1 UTF-16 unit
//   CJK          3 bytes = 1 UTF-16 unit
//   astral/emoji 4 bytes = 2 UTF-16 units (surrogate pair)
//
// bro divergence (documented, byte-domain limitation): a UTF-16 index landing
// BETWEEN the two units of a surrogate pair resolves to the preceding
// character boundary — the byte domain cannot name the middle of a code point.

const root = document.getElementById('root');

// ASCII + accented Latin + CJK + astral musical symbol + emoji + ZWJ family.
const S = 'ab' + 'éü' + '中文' + '𝄞' +
          '😀' + '👨‍👩‍👦' + 'z';

// Code-unit indices that sit on a code-point boundary (bro can name these
// exactly). Mid-surrogate indices are tested separately below.
const boundaries = [];
for (let i = 0; i < S.length; ) {
    boundaries.push(i);
    i += (S.codePointAt(i) > 0xffff) ? 2 : 1;
}
boundaries.push(S.length);

function freshText(s) {
    root.innerHTML = '';
    const t = document.createTextNode(s === undefined ? S : s);
    root.appendChild(t);
    return t;
}

// ---- length ---------------------------------------------------------------
let t = freshText();
assert(t.length === S.length,
       'text .length is UTF-16 units: expected ' + S.length + ', got ' + t.length);
assert(t.data === S, 'data round-trips');
assert(t.data.length === t.length, 'data.length matches .length');

// A comment node shares the CharacterData implementation.
const c = document.createComment(S);
assert(c.length === S.length,
       'comment .length is UTF-16 units: expected ' + S.length + ', got ' + c.length);
assert(c.substringData(0, 4) === S.substr(0, 4), 'comment substringData');

// ---- substringData exhaustively vs String.prototype.substr ----------------
for (const i of boundaries) {
    for (const j of boundaries) {
        if (j < i) continue;
        const got = t.substringData(i, j - i);
        const want = S.substr(i, j - i);
        assert(got === want,
               'substringData(' + i + ',' + (j - i) + ') expected "' + want +
               '" got "' + got + '"');
    }
}
// Count running past the end clamps, like the spec.
assert(t.substringData(2, 999) === S.substr(2), 'substringData over-long count clamps');

// ---- mutation methods land at the right position --------------------------
for (const i of boundaries) {
    t = freshText();
    t.insertData(i, 'XY');
    assert(t.data === S.slice(0, i) + 'XY' + S.slice(i),
           'insertData at ' + i + ' got "' + t.data + '"');
    assert(t.length === S.length + 2, 'length after insertData at ' + i);
}

for (const i of boundaries) {
    for (const j of boundaries) {
        if (j <= i) continue;
        t = freshText();
        t.deleteData(i, j - i);
        assert(t.data === S.slice(0, i) + S.slice(j),
               'deleteData(' + i + ',' + (j - i) + ') got "' + t.data + '"');

        t = freshText();
        t.replaceData(i, j - i, 'Qé');
        assert(t.data === S.slice(0, i) + 'Qé' + S.slice(j),
               'replaceData(' + i + ',' + (j - i) + ') got "' + t.data + '"');
    }
}

t = freshText();
t.appendData('🚀');
assert(t.data === S + '🚀', 'appendData content');
assert(t.length === S.length + 2, 'appendData grows length by 2 units for astral');

// ---- splitText ------------------------------------------------------------
for (const i of boundaries) {
    t = freshText();
    const tail = t.splitText(i);
    assert(t.data === S.slice(0, i), 'splitText(' + i + ') head "' + t.data + '"');
    assert(tail.data === S.slice(i), 'splitText(' + i + ') tail "' + tail.data + '"');
    assert(t.length + tail.length === S.length, 'splitText(' + i + ') lengths sum');
    assert(t.nextSibling === tail, 'splitText inserts tail after head');
}

// ---- documented divergence: indices inside a surrogate pair ---------------
// The astral musical symbol starts at index 6 (a+b+é+ü+中+文), so 7 is between
// its two surrogates. Spec says the index is expressible; bro resolves it to
// the preceding code-point boundary rather than producing a lone surrogate.
const astralStart = S.indexOf('\ud834');
assert(astralStart === 6, 'astral char starts at index 6, got ' + astralStart);
t = freshText();
// Chrome would hand back the lone trailing surrogate here. bro snaps the start
// back to the code-point boundary, so the whole astral character comes out —
// never an unpaired surrogate.
assert(t.substringData(astralStart + 1, 1) === S.substr(astralStart, 2),
       'mid-surrogate start snaps back to the code-point boundary, got "' +
       t.substringData(astralStart + 1, 1) + '"');
t = freshText();
t.insertData(astralStart + 1, 'M');
assert(t.data === S.slice(0, astralStart) + 'M' + S.slice(astralStart),
       'insertData mid-surrogate lands on the preceding boundary');
t = freshText();
const midTail = t.splitText(astralStart + 1);
assert(t.data === S.slice(0, astralStart) && midTail.data === S.slice(astralStart),
       'splitText mid-surrogate splits at the preceding boundary');

// ---- wholeText spans siblings in the JS string domain ---------------------
root.innerHTML = '';
const a = document.createTextNode(S);
const b = document.createTextNode('🌟');
root.appendChild(a);
root.appendChild(b);
assert(a.wholeText === S + '🌟', 'wholeText concatenates siblings');

console.log('PASS test_characterdata_utf16');
