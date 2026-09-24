// Range.cloneContents / extractContents / deleteContents / surroundContents
// with partially contained nodes (DOM standard, "clone the contents" /
// "extract").
//
// These used to be right only when both boundaries sat in one text node or
// were element offsets: there was no partially-contained-child step, so a
// range from a text node into an <em> cloned as bare text ("ect any live"),
// a range ending inside a <strong> cloned as nothing, extract left the
// document untouched, and surroundContents over a plain text range never
// returned.

const root = document.getElementById('root');

function assertEqual(got, want, msg) {
    assert(got === want, msg + ' (got ' + JSON.stringify(got instanceof Node ? got.nodeName : got) +
        ', want ' + JSON.stringify(want instanceof Node ? want.nodeName : want) + ')');
}

function ser(frag) {
    const d = document.createElement('div');
    d.appendChild(frag);
    return d.innerHTML;
}

function setup() {
    root.innerHTML =
        '<p id="q">Select any <em>live telemetry</em> here</p>' +
        '<p id="p">The <strong>DOM Range</strong> interface</p>';
    flush();
    const q = document.getElementById('q');
    const p = document.getElementById('p');
    return { q, em: q.querySelector('em'), p, strong: p.querySelector('strong') };
}

// --- cloneContents keeps the partially selected element, shallow ----------
{
    const { q, em, p, strong } = setup();
    let r = document.createRange();
    r.setStart(q.firstChild, 3);
    r.setEnd(em.firstChild, 4);
    assertEqual(ser(r.cloneContents()), 'ect any <em>live</em>', 'text@3 -> em text@4');
    assertEqual(q.innerHTML, 'Select any <em>live telemetry</em> here', 'clone leaves the source alone');

    r = document.createRange();
    r.setStart(q.firstChild, 0);
    r.setEnd(em.firstChild, 4);
    assertEqual(ser(r.cloneContents()), 'Select any <em>live</em>', 'text@0 -> em text@4');

    r = document.createRange();
    r.setStart(p.firstChild, 0);
    r.setEnd(strong.firstChild, strong.firstChild.length);
    assertEqual(ser(r.cloneContents()), 'The <strong>DOM Range</strong>',
        'text@0 -> end of strong text');

    // From inside one element into another, across a whole text node.
    r = document.createRange();
    r.setStart(em.firstChild, 5);
    r.setEnd(p.lastChild, 4);
    assertEqual(ser(r.cloneContents()),
        '<p id="q"><em>telemetry</em> here</p><p id="p">The <strong>DOM Range</strong> int</p>',
        'across two paragraphs');
}

// --- extractContents moves the selected part out ---------------------------
{
    const { q, em } = setup();
    const r = document.createRange();
    r.setStart(q.firstChild, 3);
    r.setEnd(em.firstChild, 4);
    const out = ser(r.extractContents());
    assertEqual(out, 'ect any <em>live</em>', 'extract returns the selected part');
    assertEqual(q.innerHTML, 'Sel<em> telemetry</em> here', 'extract removes it from the document');
    assert(r.collapsed, 'range collapses after extract');
    assertEqual(r.startContainer, q, 'collapses into the paragraph');
    assertEqual(r.startOffset, 1, 'between the kept text and the <em>');
}

// --- deleteContents: same tree effect, nothing returned --------------------
{
    const { p, strong } = setup();
    const r = document.createRange();
    r.setStart(p.firstChild, 1);
    r.setEnd(strong.firstChild, 3);
    r.deleteContents();
    assertEqual(p.innerHTML, 'T<strong> Range</strong> interface', 'deleteContents trims both sides');
    assert(r.collapsed, 'range collapsed after delete');
}

// --- surroundContents ------------------------------------------------------
{
    const { q } = setup();
    const t = q.firstChild;
    const r = document.createRange();
    r.setStart(t, 0);
    r.setEnd(t, 3);
    r.surroundContents(document.createElement('b'));   // used to hang
    assertEqual(q.innerHTML, '<b>Sel</b>ect any <em>live telemetry</em> here',
        'a text-only range is wrapped');
    const b = q.querySelector('b');
    assertEqual(r.startContainer, q, 'range selects the new parent (container)');
    assertEqual(r.endOffset - r.startOffset, 1, 'range selects exactly the new parent');
    assertEqual(q.childNodes[r.startOffset], b, 'range selects the <b>');
}
{
    const { q, em } = setup();
    const r = document.createRange();
    r.setStart(q.firstChild, 3);
    r.setEnd(em.firstChild, 4);
    let name = null;
    try {
        r.surroundContents(document.createElement('mark'));
    } catch (e) {
        name = e.name;
    }
    assertEqual(name, 'InvalidStateError', 'partially selecting <em> throws InvalidStateError');
    assertEqual(q.innerHTML, 'Select any <em>live telemetry</em> here', 'and changes nothing');
}
{
    // Element-offset ranges still wrap.
    const { p } = setup();
    const r = document.createRange();
    r.setStart(p, 1);
    r.setEnd(p, 2);
    r.surroundContents(document.createElement('i'));
    assertEqual(p.innerHTML, 'The <i><strong>DOM Range</strong></i> interface', 'element-offset wrap');
}

// --- insertNode into a text node splits it; live ranges follow -------------
{
    const { q } = setup();
    const r = document.createRange();
    r.setStart(q.firstChild, 6);
    r.setEnd(q.firstChild, 6);
    const hr = document.createElement('span');
    hr.textContent = '|';
    r.insertNode(hr);
    assertEqual(q.innerHTML, 'Select<span>|</span> any <em>live telemetry</em> here', 'insert splits the text');
    assertEqual(r.toString(), '|', 'a collapsed range grows over the inserted node');

    let name = null;
    try { r.insertNode(q); } catch (e) { name = e.name; }
    assertEqual(name, 'HierarchyRequestError', 'inserting an ancestor throws');
}

// A range whose end is an element offset in a parent shifts when a node is
// inserted before that offset (DOM "insert" live-range update).
{
    const { p } = setup();
    const r = document.createRange();
    r.setStart(p, 0);
    r.setEnd(p, 2);
    p.insertBefore(document.createElement('br'), p.firstChild);
    assertEqual(r.endOffset, 3, 'end offset past the insertion point moves right');
    assertEqual(r.startOffset, 0, 'start offset AT the insertion point stays');
}

console.log('test_range_partial_contents: done');
