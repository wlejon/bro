// A press inside a contenteditable puts the caret inside THAT host.
//
// hitTestText resolves a press that misses every text run to the nearest run
// instead of to nothing, which is what makes clicking the padding below a
// paragraph place a caret at all. Unscoped, though, "nearest" was measured
// across the whole document — so a press in the blank right-hand part of a
// wide editable box landed on whatever text happened to be closest, which for
// a box with a readout underneath it is the line below.
//
// The caret then sat in a node OUTSIDE the editable host: invisible, and every
// keystroke went nowhere. From the user's side the box simply refused to take
// input anywhere except directly on its text.

const root = document.getElementById('root');

function rects(host) {
    const t = host.firstChild;
    const r = document.createRange();
    r.setStart(t, 0);
    r.setEnd(t, t.data.length);
    return { host: host.getBoundingClientRect(), text: r.getBoundingClientRect() };
}

// Click at (x, y) and return what the selection anchored to.
function clickAt(x, y) {
    mouseDown(x, y);
    mouseUp(x, y);
    flush();
    const s = window.getSelection();
    return {
        rangeCount: s.rangeCount,
        node: s.anchorNode,
        offset: s.rangeCount ? s.anchorOffset : -1,
    };
}

// --- the shape that broke it: a wide editable with a sibling right below ----
//
// Load-bearing: the neighbour has to be close underneath and extend further
// right than the editable's own text, because that is what makes it the
// nearest run for a press in the blank area. Without it the bug is invisible.
{
    root.innerHTML =
        '<div id="host" contenteditable="true" ' +
        'style="width:800px;padding:6px 9px;font-family:Arial;font-size:20px">' +
        'abc def</div>' +
        '<div id="below" style="width:800px;font-family:Arial;font-size:20px">' +
        'a much longer neighbouring line of text that reaches further right</div>';
    flush();

    const host = document.getElementById('host');
    const below = document.getElementById('below');
    const g = rects(host);
    const y = (Math.max(g.host.y, g.text.y) +
               Math.min(g.host.bottom, g.text.bottom)) / 2;
    const end = host.firstChild.data.length;

    // On the text: the easy case, which worked before and must keep working.
    const onText = clickAt(g.text.x + 2, y);
    assert(host.contains(onText.node),
           'a press on the text anchors inside the host');

    // Past the end of the text but still inside the box — the reported bug.
    for (const dx of [20, 200, 600]) {
        const hit = clickAt(g.text.right + dx, y);
        assert(hit.rangeCount === 1,
               `a press ${dx}px right of the text still places a caret`);
        assert(host.contains(hit.node),
               `and it lands INSIDE the editable host, not in the neighbouring ` +
               `text below it (dx=${dx}, anchored in ` +
               `${below.contains(hit.node) ? '#below' : 'some other node'})`);
        assert(hit.offset === end,
               `at the end of the text (offset ${hit.offset}, want ${end}) — ` +
               'past the last character means after it');
    }
}

// --- and typing there actually lands ----------------------------------------
//
// The assertion that matters to a user. A caret in the wrong node is only
// detectable as "typing does nothing", so this states it that way.
{
    root.innerHTML =
        '<div id="host2" contenteditable="true" ' +
        'style="width:800px;padding:6px 9px;font-family:Arial;font-size:20px">' +
        'abc</div>' +
        '<div style="width:800px;font-family:Arial;font-size:20px">' +
        'a much longer neighbouring line of text that reaches further right</div>';
    flush();

    const host = document.getElementById('host2');
    const g = rects(host);
    const y = (Math.max(g.host.y, g.text.y) +
               Math.min(g.host.bottom, g.text.bottom)) / 2;

    clickAt(g.text.right + 400, y);
    textInput('Z');
    flush();
    assert(host.textContent === 'abcZ',
           'typing after a press right of the text appends to the host, got ' +
           JSON.stringify(host.textContent));
}

// --- the scope must not move the coordinate space ---------------------------
//
// Restricting the search to a subtree and RESTARTING the walk at that subtree
// look equivalent and are not: the walk accumulates offsets from wherever it
// starts, so handing it the subtree returns runs positioned relative to the
// subtree while the query point is still absolute. The error is the host's own
// distance from the origin, which is why it hides completely in a host near
// the top-left corner — the first version of this test had one and passed.
//
// So: push the host far down and far right, and assert the caret lands on the
// character actually under the pointer rather than merely inside the host.
{
    root.innerHTML =
        '<div style="padding-top:900px;padding-left:220px">' +
        '<div id="host3" contenteditable="true" ' +
        'style="width:700px;padding:6px 9px;font-family:Arial;font-size:20px">' +
        'abcdefghij</div>' +
        '<div style="width:700px;font-family:Arial;font-size:20px">' +
        'a much longer neighbouring line of text that reaches further right</div>' +
        '</div>';
    flush();

    const host = document.getElementById('host3');
    const t = host.firstChild;
    const g = rects(host);
    const y = (Math.max(g.host.y, g.text.y) +
               Math.min(g.host.bottom, g.text.bottom)) / 2;
    assert(g.text.x > 200 && g.text.y > 900,
           `the host really is far from the origin (${g.text.x}, ${g.text.y}) — ` +
           'otherwise this test cannot see the bug it exists for');

    // The middle of a character must resolve to that character's own boundary.
    for (const i of [1, 4, 7]) {
        const r = document.createRange();
        r.setStart(t, i);
        r.setEnd(t, i + 1);
        const b = r.getBoundingClientRect();
        const hit = clickAt((b.x + b.right) / 2, y);
        assert(host.contains(hit.node),
               `a press on character ${i} stays in the host`);
        assert(hit.offset === i || hit.offset === i + 1,
               `and resolves to character ${i}, not ${hit.offset} — a constant ` +
               'offset error here means the hit test is reading run coordinates ' +
               'in a different space from the one the press is measured in');
    }
}

// --- a press outside every editable still reaches ordinary text --------------
//
// The scope must not become a cage: selecting normal page text is unaffected,
// because there is no editing host to scope to.
{
    root.innerHTML =
        '<p id="plain" style="width:600px;font-family:Arial;font-size:20px">' +
        'ordinary paragraph text</p>';
    flush();

    const p = document.getElementById('plain');
    const g = rects(p);
    const hit = clickAt(g.text.x + 20, g.text.y + g.text.height / 2);
    assert(hit.rangeCount === 1 && p.contains(hit.node),
           'a press on non-editable text still selects into it');
}

root.innerHTML = '';
