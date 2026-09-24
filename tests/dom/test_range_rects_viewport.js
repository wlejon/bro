// Range.getClientRects / getBoundingClientRect are viewport rects like an
// element's, and they report what is selected rather than what is visible.
//
//  1. Text scrolled out of an overflow:auto box used to come back with the
//     right top and width 0: the rects were clipped to the scroller (the
//     highlight painter's need, not CSSOM's), every band was clipped away, and
//     the caret fallback answered instead.
//  2. With bro.menu shown, every range rect was 28px (the menu bar) too low —
//     the binding added the menu inset that element rects and clientY already
//     leave out.
//  3. A range over a wrapped inline element reports the element's own boxes
//     one per line fragment, as Element.getClientRects does, not one union.

const root = document.getElementById('root');
const near = (a, b, tol = 0.5) => Math.abs(a - b) <= tol;

// --- 1. scrolled out of a scroller ----------------------------------------
{
    root.innerHTML =
        '<div id="scr" style="height:200px;overflow-y:auto;font:20px Arial">' +
        '<div>near text</div><div style="height:1000px"></div>' +
        '<div id="far">far text</div></div>';
    flush();
    const far = document.getElementById('far');
    const r = document.createRange();
    r.setStart(far.firstChild, 0);
    r.setEnd(far.firstChild, 4);
    const hidden = r.getBoundingClientRect();
    const rects = r.getClientRects();
    assert(hidden.width > 10, 'out-of-view text has its width (got ' + hidden.width + ')');
    assert(rects.length === 1 && rects[0].width > 10, 'and one band in getClientRects');
    assert(near(hidden.top, far.getBoundingClientRect().top, 3),
        'at the element\'s position (' + hidden.top + ' vs ' + far.getBoundingClientRect().top + ')');

    document.getElementById('scr').scrollTop = 1000;
    flush();
    const shown = r.getBoundingClientRect();
    assert(near(shown.width, hidden.width), 'same width once scrolled into view (' +
        shown.width + ' vs ' + hidden.width + ')');
}

// --- 2. the menu bar does not shift range rects ----------------------------
{
    root.innerHTML = '<p id="c" style="font:20px Arial">Select any segment</p>';
    flush();
    const c = document.getElementById('c');
    const r = document.createRange();
    r.setStart(c.firstChild, 0);
    r.setEnd(c.firstChild, 6);
    const before = r.getBoundingClientRect().top - c.getBoundingClientRect().top;
    const caret = document.createRange();
    caret.setStart(c.firstChild, 2);
    const caretBefore = caret.getBoundingClientRect().top - c.getBoundingClientRect().top;

    bro.menu.show();
    bro.menu.set([{ label: 'File', items: [{ id: 'q', label: 'Quit' }] }]);
    flush();
    advanceTime(20);
    flush();
    const after = r.getBoundingClientRect().top - c.getBoundingClientRect().top;
    const caretAfter = caret.getBoundingClientRect().top - c.getBoundingClientRect().top;
    assert(near(before, after), 'range top relative to its <p> unchanged by the menu bar (' +
        before + ' -> ' + after + ')');
    assert(near(r.getClientRects()[0].top - c.getBoundingClientRect().top, after),
        'getClientRects agrees');
    assert(near(caretBefore, caretAfter), 'collapsed caret too (' + caretBefore + ' -> ' + caretAfter + ')');
    bro.menu.hide();
    flush();
}

// --- 3. inline fragments ----------------------------------------------------
{
    root.innerHTML =
        '<div style="width:300px;font:20px Arial;line-height:30px">Some lead text then ' +
        '<span id="s" style="padding:0 4px;border:2px solid red">a wrapped span that runs over ' +
        'several lines of text</span> and after.</div>';
    flush();
    const s = document.getElementById('s');
    const frags = Array.from(s.getClientRects());
    assert(frags.length === 3, 'the span has one client rect per line (got ' + frags.length + ')');
    for (let i = 1; i < frags.length; i++) {
        assert(frags[i].top > frags[i - 1].top + 20, 'fragment ' + i + ' is on the next line');
    }

    const text = document.createRange();
    text.selectNodeContents(s.firstChild);
    const bands = Array.from(text.getClientRects());
    assert(bands.length === 3, 'three text bands (got ' + bands.length + ')');
    // First fragment starts at the padding+border before the text; the last
    // ends after it; vertical padding+border wrap every fragment.
    assert(near(frags[0].left, bands[0].left - 6, 1), 'first fragment starts 6px before its text');
    assert(near(frags[2].right, bands[2].right + 6, 1), 'last fragment ends 6px after its text');
    assert(near(frags[1].left, bands[1].left, 1) && near(frags[1].right, bands[1].right, 1),
        'a middle fragment has no inline padding');
    for (let i = 0; i < 3; i++) {
        assert(near(frags[i].top, bands[i].top - 2, 1) && near(frags[i].height, bands[i].height + 4, 1),
            'fragment ' + i + ' wraps its text band vertically by the border');
    }

    const whole = document.createRange();
    whole.selectNode(s);
    const all = Array.from(whole.getClientRects());
    assert(all.length === 6, 'selectNode(span): 3 element fragments + 3 text bands (got ' + all.length + ')');
    assert(near(all[0].left, frags[0].left) && near(all[2].top, frags[2].top),
        'the element part is the per-line fragments');
}

console.log('test_range_rects_viewport: done');
