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
    const ltr = runRects('<p style="width:400px">' + ALEF + ' ' + DALET + '</p>');
    assert(ltr.length === 2, 'two Hebrew words make two runs, got ' + ltr.length);
    assert(ascendingX(ltr), 'runs are recorded left to right');
    // ALEF is the wider word; after reversal it must be on the RIGHT.
    assert(ltr[1].w > ltr[0].w,
           'under an LTR base the first Hebrew word ends up rightmost: ' +
           JSON.stringify(ltr));

    const rtl = runRects('<p dir="rtl" style="width:400px">' + ALEF + ' ' + DALET + '</p>');
    assert(rtl.length === 2, 'two runs with an RTL base');
    assert(rtl[1].w > rtl[0].w,
           'under an RTL base the first Hebrew word is still rightmost: ' +
           JSON.stringify(rtl));
    // text-align: start resolves to right for an RTL block.
    assert(rtl[1].x + rtl[1].w > 380,
           'an RTL paragraph is flush to the right edge, got ' +
           (rtl[1].x + rtl[1].w));
}

// --- Latin around an RTL island: only the island reverses --------------------
{
    const rects = runRects(
        '<p style="width:400px">start ' + ALEF + ' ' + DALET + ' end</p>');
    // start, ALEF, DALET, end -> start, DALET, ALEF, end
    assert(rects.length === 4, 'four runs, got ' + rects.length);
    assert(rects[0].x < 1, 'the LTR text still begins at the left edge');
    assert(rects[2].w > rects[1].w,
           'the Hebrew island reversed within the LTR line: ' +
           JSON.stringify(rects));
}

// --- mixed direction with numbers -------------------------------------------
//
// Digits after Arabic resolve one level above it, which means they keep their
// own left-to-right order inside a line that is otherwise reversed.
{
    const rects = runRects(
        '<p dir="rtl" style="width:400px">' + ARABIC + ' abc 123 ' + ALEF + '</p>');
    assert(rects.length === 4, 'four runs, got ' + rects.length);
    assert(ascendingX(rects), 'runs are recorded left to right');
    // Visually: ALEF | abc | 123 | ARABIC. The Latin island keeps its order,
    // so "abc" is left of "123" even though the line reads right to left.
    const abc = rects[1];
    const digits = rects[2];
    assert(abc.x < digits.x,
           'the LTR island keeps its internal order inside an RTL line: ' +
           JSON.stringify(rects));
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
    const rects = runRects('<p style="width:400px">one two three</p>');
    assert(rects.length === 3, 'three Latin runs');
    assert(ascendingX(rects), 'Latin runs stay in source order');
    assert(rects[0].x < 1, 'and start at the left edge');
}

root.innerHTML = '';
