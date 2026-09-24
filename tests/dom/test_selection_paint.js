// The painted selection highlight covers what is selected and nothing else.
//
//  - Text inside an element that became display:none kept its last text runs
//    in the layout tree, and the highlight painted them at the page origin,
//    over whatever is there now.
//  - A selection ending at an element offset — (p, 2), just before the
//    trailing " tail" text — resolved that end into the text AFTER the
//    boundary, at its end, so the highlight ran to the end of the paragraph
//    while toString() correctly stopped at "second".

const root = document.getElementById('root');
const sel = getSelection();

// The highlight is a translucent accent blue over the text; unselected text
// and background are greys. "Blue" = blue channel well above red.
function blueAt(x, y) {
    const p = getPixel(Math.round(x), Math.round(y));
    return p.b - p.r > 30;
}
function rectOf(node, from, to) {
    const r = document.createRange();
    r.setStart(node, from);
    r.setEnd(node, to);
    return r.getBoundingClientRect();
}

// --- element-offset end -----------------------------------------------------
{
    root.innerHTML = '<p id="p1" style="font:28px Arial;background:#fff;color:#000">' +
        'first <b>second</b> tail</p>';
    flush();
    const p1 = document.getElementById('p1');
    const head = p1.firstChild;
    const tail = p1.lastChild;
    const r = document.createRange();
    r.setStart(head, 4);
    r.setEnd(p1, 2);
    sel.removeAllRanges();
    sel.addRange(r);
    advanceTime(20);
    flush();
    assert(String(sel) === 't second', 'toString stops at the boundary (' + String(sel) + ')');

    const inB = rectOf(p1.querySelector('b').firstChild, 1, 5);
    assert(blueAt(inB.left + inB.width / 2, inB.top + inB.height / 2), '"second" is highlighted');
    const inTail = rectOf(tail, 1, 5);
    assert(!blueAt(inTail.left + inTail.width / 2, inTail.top + inTail.height / 2),
        '" tail" after the element-offset end is not highlighted');
    const headText = rectOf(head, 0, 3);
    assert(!blueAt(headText.left + headText.width / 2, headText.top + headText.height / 2),
        '"fir" before the start is not highlighted');
}

// --- a selection inside a display:none element ----------------------------
{
    root.innerHTML =
        '<div style="background:#fff;font:28px Arial">' +
        '<p id="plain">some plain text here</p></div>';
    flush();
    const plain = document.getElementById('plain');
    const t = plain.firstChild;
    const where = rectOf(t, 0, 4);
    sel.setBaseAndExtent(t, 0, t, 4);
    advanceTime(20);
    flush();
    assert(blueAt(where.left + where.width / 2, where.top + where.height / 2),
        'the visible selection is painted');

    plain.style.display = 'none';
    advanceTime(20);
    flush();
    // Where the words were, and the page origin the stale runs used to be
    // painted at.
    const probes = [
        [where.left + where.width / 2, where.top + where.height / 2],
        [where.left + 5, where.top + 5],
        [3, 3], [20, 10], [40, 15],
    ];
    for (const [x, y] of probes) {
        assert(!blueAt(x, y), 'no highlight left at (' + x + ',' + y + ') once hidden');
    }
    const rects = sel.getRangeAt(0).getClientRects();
    assert(Array.from(rects).every(r => r.width === 0),
        'and the hidden range has no text boxes');
}

console.log('test_selection_paint: done');
