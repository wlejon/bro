// Bidi display: RTL text lands in the right visual order on the line.
//
// The conformance corpus (test_bidi_conformance.js) proves the algorithm is
// right. This proves the algorithm is actually reaching layout — that a
// paragraph of Hebrew arrives at the renderer with its words in visual order
// rather than in the order they appear in the source.
//
// Positions are read through Range.getClientRects, which emits one rect per
// placed run in the order the runs were laid out. So the FIRST rect is the
// leftmost run, and comparing rect widths identifies which word landed where
// without needing to know font metrics.

const root = document.getElementById('root');

// Three Hebrew letters and three different ones, so the two words have
// distinguishable widths.
const ALEF = String.fromCharCode(0x05D0, 0x05D1, 0x05D2);   // aleph bet gimel
const DALET = String.fromCharCode(0x05D3, 0x05D4);          // dalet he — narrower
const ARABIC = String.fromCharCode(0x0645, 0x0631, 0x062D, 0x0628, 0x0627);

// The rect of one word inside the paragraph, by UTF-16 offsets. Band counts
// say how the line divides into direction runs; this says where a particular
// word ended up, which is what "the order reversed" actually means.
function wordRect(html, a, z) {
    root.innerHTML = html;
    flush();
    const t = root.firstChild.firstChild;
    const r = document.createRange();
    r.setStart(t, a);
    r.setEnd(t, z);
    const b = r.getBoundingClientRect();
    return { x: b.x, w: b.width };
}

function runRects(html) {
    root.innerHTML = html;
    flush();
    const p = root.firstChild;
    const t = p.firstChild;
    const r = document.createRange();
    r.setStart(t, 0);
    r.setEnd(t, t.data.length);
    const rects = r.getClientRects();
    const out = [];
    for (let i = 0; i < rects.length; i++) {
        out.push({ x: rects[i].x, w: rects[i].width });
    }
    return out;
}

function ascendingX(rects) {
    for (let i = 1; i < rects.length; i++) {
        if (rects[i].x < rects[i - 1].x) return false;
    }
    return true;
}

// --- a whole-run selection must have a real rect, in either direction --------
//
// An RTL run's caret positions run right-to-left, so the naive "clamp the
// end-of-text caret to the run width" gives both edges of a full selection the
// same x and the rect collapses. That bug is invisible until something asks.
{
    const rects = runRects('<p style="width:400px">' + ALEF + '</p>');
    assert(rects.length === 1, 'one run for one Hebrew word, got ' + rects.length);
    assert(rects[0].w > 1, 'the Hebrew run has a real width, got ' + rects[0].w);
}
{
    const rects = runRects('<p style="width:400px">' + ARABIC + '</p>');
    assert(rects.length === 1, 'one run for one Arabic word');
    assert(rects[0].w > 1, 'the Arabic run has a real width, got ' + rects[0].w);
}

// --- two RTL words swap, under either base direction -------------------------
{
    // Two Hebrew words separated by a space are ONE band, not two: the space
    // is a UAX #9 neutral between two right-to-left runs, so it resolves RTL
    // and the whole span is a single direction run. Chromium reports one rect
    // here too. (This used to assert two, which described how layout happens
    // to split runs at wrap opportunities rather than anything about bidi.)
    const html = '<p style="width:400px">' + ALEF + ' ' + DALET + '</p>';
    const ltr = runRects(html);
    assert(ltr.length === 1, 'two Hebrew words are one RTL band, got ' + ltr.length);

    // The reversal itself, stated where it is actually visible: ALEF is
    // logically first and must be drawn to the RIGHT of DALET.
    const alef = wordRect(html, 0, 3);
    const dalet = wordRect(html, 4, 6);
    assert(alef.x > dalet.x,
           'under an LTR base the first Hebrew word ends up rightmost: ' +
           JSON.stringify({ alef, dalet }));
    assert(alef.w > dalet.w, 'and ALEF is the wider of the two');

    const rtlHtml = '<p dir="rtl" style="width:400px">' + ALEF + ' ' + DALET + '</p>';
    const rtl = runRects(rtlHtml);
    assert(rtl.length === 1, 'one band with an RTL base too, got ' + rtl.length);
    assert(wordRect(rtlHtml, 0, 3).x > wordRect(rtlHtml, 4, 6).x,
           'under an RTL base the first Hebrew word is still rightmost');
    // text-align: start resolves to right for an RTL block.
    assert(rtl[0].x + rtl[0].w > 380,
           'an RTL paragraph is flush to the right edge, got ' +
           (rtl[0].x + rtl[0].w));
}

// --- Latin around an RTL island: only the island reverses --------------------
{
    const html = '<p style="width:400px">start ' + ALEF + ' ' + DALET + ' end</p>';
    const rects = runRects(html);
    // Three bands, not four: "start " | the whole Hebrew island | " end". Each
    // space takes the base direction where it separates opposite runs, and RTL
    // where it sits between the two Hebrew words — so the island's internal
    // space belongs to the island. Chromium reports the same three.
    assert(rects.length === 3, 'three direction bands, got ' + rects.length);
    assert(rects[0].x < 1, 'the LTR text still begins at the left edge');
    // The island reversed: ALEF (logically first) is right of DALET.
    // "start " is 6 UTF-16 units, ALEF 3, a space, DALET 2.
    assert(wordRect(html, 6, 9).x > wordRect(html, 10, 12).x,
           'the Hebrew island reversed within the LTR line: ' +
           JSON.stringify(rects));
}

// --- mixed direction with numbers -------------------------------------------
//
// Digits after Arabic resolve one level above it, which means they keep their
// own left-to-right order inside a line that is otherwise reversed.
{
    const html =
        '<p dir="rtl" style="width:400px">' + ARABIC + ' abc 123 ' + ALEF + '</p>';
    const rects = runRects(html);
    // Three bands: ALEF | "abc 123" | ARABIC. The Latin text and the digits
    // are one left-to-right island — the space between them is a neutral with
    // the same direction on both sides — so they do not divide. Chromium
    // reports the same three.
    assert(rects.length === 3, 'three direction bands, got ' + rects.length);
    assert(ascendingX(rects), 'runs are recorded left to right');
    // The island keeps its internal order: "abc" is left of "123" even though
    // the line reads right to left. ARABIC is 5 units, then a space at 5,
    // "abc" at 6-9, a space, "123" at 10-13.
    const abc = wordRect(html, 6, 9);
    const digits = wordRect(html, 10, 13);
    assert(abc.x < digits.x,
           'the LTR island keeps its internal order inside an RTL line: ' +
           JSON.stringify({ abc, digits }));
}

// --- bidi control characters -------------------------------------------------
//
// An RLM after Latin makes the following neutral run right-to-left. The
// controls themselves must not occupy any width: a font that has no glyph for
// them would otherwise contribute a .notdef box each.
{
    const plain = bro.text.shape('abc', { family: 'Arial', size: 16 });
    const marks = String.fromCharCode(0x200E) + String.fromCharCode(0x200F) +
                  String.fromCharCode(0x202A) + String.fromCharCode(0x202C);
    const withControls = bro.text.shape('a' + marks + 'bc',
                                        { family: 'Arial', size: 16 });
    assert(Math.abs(plain.width - withControls.width) < 0.01,
           'bidi controls are zero-width: ' + plain.width + ' vs ' +
           withControls.width);
}

// --- an explicit embedding changes the resolved order ------------------------
{
    const RLE = String.fromCharCode(0x202B);
    const PDF = String.fromCharCode(0x202C);
    // Six code points: x RLE a b PDF y.
    const inside = bro.text.bidi('x' + RLE + 'ab' + PDF + 'y', 'ltr');
    assert(inside.levels.length === 6,
           'one level per code point, got ' + inside.levels.length);
    assert(inside.levels[0] === 0, 'text before RLE stays at the base level');
    assert(inside.levels[2] === 2 && inside.levels[3] === 2,
           'Latin inside an RTL embedding sits at level 2, got ' +
           inside.levels.join(' '));
    assert(inside.levels[5] === 0,
           'PDF pops back to the base level, got ' + inside.levels.join(' '));
}

// --- dir="auto" picks the paragraph direction from the first strong char -----
{
    const heb = bro.text.bidi(ALEF + ' abc', 'auto');
    assert(heb.paragraphLevel === 1,
           'a paragraph starting with Hebrew is RTL, got ' + heb.paragraphLevel);
    const lat = bro.text.bidi('abc ' + ALEF, 'auto');
    assert(lat.paragraphLevel === 0,
           'a paragraph starting with Latin is LTR, got ' + lat.paragraphLevel);
    const none = bro.text.bidi('123 !?', 'auto');
    assert(none.paragraphLevel === 0,
           'no strong character means LTR, got ' + none.paragraphLevel);
}

// --- plain Latin is untouched ------------------------------------------------
//
// The 99% case. Reordering must be a no-op here, not merely a correct one.
{
    const html = '<p style="width:400px">one two three</p>';
    const rects = runRects(html);
    // One band: unidirectional text is one direction run however many words
    // it has. Chromium reports one rect here too.
    assert(rects.length === 1, 'one Latin band, got ' + rects.length);
    assert(rects[0].x < 1, 'and it starts at the left edge');
    // Source order preserved, checked where it is visible: each word is left
    // of the next.
    const one = wordRect(html, 0, 3);
    const two = wordRect(html, 4, 7);
    const three = wordRect(html, 8, 13);
    assert(one.x < two.x && two.x < three.x,
           'Latin words stay in source order: ' +
           JSON.stringify({ one, two, three }));
}

root.innerHTML = '';
