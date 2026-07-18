// Range and Selection text offsets must speak UTF-16 code units, matching the
// JS string domain, not bro's internal UTF-8 byte offsets. Oracle throughout:
// what the equivalent JS string operation produces.
//
// Note offsets on ELEMENT containers are child indices and stay untouched —
// only offsets into Text/Comment containers are converted.

const root = document.getElementById('root');

const S = 'ab' + 'éü' + '中文' + '𝄞' + '😀' + '👨‍👩‍👦' + 'z';

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

// ---- setStart/setEnd round-trip and toString ------------------------------
let t = freshText();
for (const i of boundaries) {
    for (const j of boundaries) {
        if (j < i) continue;
        const r = document.createRange();
        r.setStart(t, i);
        r.setEnd(t, j);
        assert(r.startOffset === i,
               'startOffset round-trip: set ' + i + ' got ' + r.startOffset);
        assert(r.endOffset === j,
               'endOffset round-trip: set ' + j + ' got ' + r.endOffset);
        assert(r.collapsed === (i === j), 'collapsed at (' + i + ',' + j + ')');
        assert(r.toString() === S.slice(i, j),
               'range.toString() (' + i + ',' + j + ') expected "' + S.slice(i, j) +
               '" got "' + r.toString() + '"');
    }
}

// selectNodeContents covers the whole text in code units.
{
    const r = document.createRange();
    r.selectNodeContents(t);
    assert(r.startOffset === 0, 'selectNodeContents start 0');
    assert(r.endOffset === S.length,
           'selectNodeContents end is UTF-16 length ' + S.length +
           ', got ' + r.endOffset);
    assert(r.toString() === S, 'selectNodeContents toString');
}

// Out-of-range clamps to the code-unit length, not the byte length.
{
    const r = document.createRange();
    r.setStart(t, 0);
    r.setEnd(t, 9999);
    assert(r.endOffset === S.length,
           'end clamps to UTF-16 length, got ' + r.endOffset);
}

// ---- element-container offsets are child indices, NOT converted -----------
{
    root.innerHTML = '<span>😀</span><span>中</span><span>x</span>';
    flush();
    const r = document.createRange();
    r.setStart(root, 1);
    r.setEnd(root, 3);
    assert(r.startOffset === 1 && r.endOffset === 3,
           'child-index offsets pass through unconverted, got (' +
           r.startOffset + ',' + r.endOffset + ')');
    assert(r.toString() === '中x', 'element-container range text, got "' + r.toString() + '"');
}

// ---- cloneContents / extractContents cut at the right place ---------------
for (const i of boundaries) {
    for (const j of boundaries) {
        if (j <= i) continue;
        t = freshText();
        const r = document.createRange();
        r.setStart(t, i);
        r.setEnd(t, j);
        const frag = r.cloneContents();
        assert(frag.textContent === S.slice(i, j),
               'cloneContents(' + i + ',' + j + ') expected "' + S.slice(i, j) +
               '" got "' + frag.textContent + '"');
        // Content must survive the clone. (bro divergence, orthogonal to
        // offsets and pre-existing: cloneContents splits the source text node
        // at both boundaries instead of cloning without touching the tree, so
        // `t.data` is only the head afterwards. The rendered text is intact.)
        assert(root.textContent === S, 'cloneContents preserves the text content');

        t = freshText();
        const r2 = document.createRange();
        r2.setStart(t, i);
        r2.setEnd(t, j);
        const cut = r2.extractContents();
        assert(cut.textContent === S.slice(i, j),
               'extractContents(' + i + ',' + j + ') content');
        assert(root.textContent === S.slice(0, i) + S.slice(j),
               'extractContents(' + i + ',' + j + ') remainder "' + root.textContent + '"');
    }
}

// ---- deleteContents -------------------------------------------------------
for (const i of boundaries) {
    for (const j of boundaries) {
        if (j <= i) continue;
        t = freshText();
        const r = document.createRange();
        r.setStart(t, i);
        r.setEnd(t, j);
        r.deleteContents();
        assert(root.textContent === S.slice(0, i) + S.slice(j),
               'deleteContents(' + i + ',' + j + ') got "' + root.textContent + '"');
    }
}

// ---- insertNode splits at a code-unit offset ------------------------------
{
    const at = S.indexOf('😀');
    t = freshText();
    const r = document.createRange();
    r.setStart(t, at);
    r.setEnd(t, at);
    const mark = document.createElement('b');
    mark.textContent = '|';
    r.insertNode(mark);
    assert(root.textContent === S.slice(0, at) + '|' + S.slice(at),
           'insertNode at astral boundary got "' + root.textContent + '"');
}

// ---- comparePoint / isPointInRange ----------------------------------------
{
    t = freshText();
    const lo = boundaries[2], hi = boundaries[6];
    const r = document.createRange();
    r.setStart(t, lo);
    r.setEnd(t, hi);
    assert(r.comparePoint(t, boundaries[1]) === -1, 'comparePoint before start');
    assert(r.comparePoint(t, boundaries[4]) === 0, 'comparePoint inside');
    assert(r.comparePoint(t, S.length) === 1, 'comparePoint after end');
    assert(r.isPointInRange(t, boundaries[4]) === true, 'isPointInRange inside');
    assert(r.isPointInRange(t, boundaries[1]) === false, 'isPointInRange before');
}

// ---- surroundContents -----------------------------------------------------
{
    const i = S.indexOf('中'), j = i + 2;
    t = freshText();
    const r = document.createRange();
    r.setStart(t, i);
    r.setEnd(t, j);
    const wrap = document.createElement('em');
    r.surroundContents(wrap);
    assert(wrap.textContent === S.slice(i, j),
           'surroundContents wraps the right slice, got "' + wrap.textContent + '"');
    assert(root.textContent === S, 'surroundContents preserves total text');
}

// ---- Selection offsets round-trip through the DOM API ---------------------
{
    t = freshText();
    const sel = window.getSelection();
    for (const i of boundaries) {
        for (const j of boundaries) {
            sel.removeAllRanges();
            sel.setBaseAndExtent(t, i, t, j);
            assert(sel.anchorOffset === i,
                   'anchorOffset round-trip: set ' + i + ' got ' + sel.anchorOffset);
            assert(sel.focusOffset === j,
                   'focusOffset round-trip: set ' + j + ' got ' + sel.focusOffset);
            const lo = Math.min(i, j), hi = Math.max(i, j);
            assert(sel.toString() === S.slice(lo, hi),
                   'selection text (' + i + ',' + j + ') expected "' +
                   S.slice(lo, hi) + '" got "' + sel.toString() + '"');
            const gr = sel.getRangeAt(0);
            assert(gr.startOffset === lo && gr.endOffset === hi,
                   'getRangeAt offsets are UTF-16 (' + gr.startOffset + ',' +
                   gr.endOffset + ') want (' + lo + ',' + hi + ')');
        }
    }

    // collapse / extend
    const at = S.indexOf('👨');
    sel.collapse(t, at);
    assert(sel.anchorOffset === at && sel.focusOffset === at,
           'collapse offset round-trip, got ' + sel.anchorOffset);
    assert(sel.isCollapsed, 'collapsed after collapse()');
    sel.extend(t, S.length);
    assert(sel.focusOffset === S.length,
           'extend focusOffset ' + sel.focusOffset + ' want ' + S.length);
    assert(sel.toString() === S.slice(at), 'extend selection text');
}

// ---- addRange feeds UTF-16 offsets straight back --------------------------
{
    t = freshText();
    const sel = window.getSelection();
    sel.removeAllRanges();
    const i = S.indexOf('😀'), j = i + 2;
    const r = document.createRange();
    r.setStart(t, i);
    r.setEnd(t, j);
    sel.addRange(r);
    assert(sel.anchorOffset === i && sel.focusOffset === j,
           'addRange offsets stay UTF-16, got (' + sel.anchorOffset + ',' +
           sel.focusOffset + ')');
    assert(sel.toString() === '😀', 'addRange selection text "' + sel.toString() + '"');
}

// ---- contenteditable typing at an astral caret ----------------------------
// The typing pipeline is byte-internal; the caret we set through the JS
// Selection API must convert exactly once (a double conversion lands the
// insertion in the wrong place or mid-sequence).
{
    root.innerHTML = '<div id="ed" contenteditable="true"></div>';
    flush();
    const ed = document.getElementById('ed');
    const tn = document.createTextNode(S);
    ed.appendChild(tn);
    flush();
    const rect = ed.getBoundingClientRect();
    click(rect.left + 5, rect.top + 5);

    const at = S.indexOf('😀');
    const sel = window.getSelection();
    sel.removeAllRanges();
    sel.collapse(tn, at);
    textInput('Z');
    flush();
    assert(ed.textContent === S.slice(0, at) + 'Z' + S.slice(at),
           'typed char lands at the UTF-16 caret, got "' + ed.textContent + '"');
    // And the caret reported back is still UTF-16.
    assert(window.getSelection().anchorOffset === at + 1,
           'caret after typing is at ' + window.getSelection().anchorOffset +
           ', want ' + (at + 1));
}

console.log('PASS test_range_utf16');
