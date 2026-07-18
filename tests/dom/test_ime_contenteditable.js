// IME composition on contenteditable hosts (imeCompose / imeCommit /
// imeCancel through the real engine path). Parity with the input/textarea
// implementation, DOM-splice flavored:
//   - the preedit is spliced provisionally into the text node at the caret
//     (browser-observable: textContent shows it), replaced on every update
//   - compositionstart/update/end and input(insertCompositionText) fire on
//     the HOST element in Chrome's observable order
//   - cancel removes the preedit and restores the pre-composition DOM
//   - commit finalizes as ONE coherent splice (the DOM stays one text node)
//   - caret moves / clicks / Tab / programmatic focus mid-composition COMMIT
//   - at an element boundary the preedit lands in a text node created by the
//     same insertion rule regular contenteditable typing uses
// NOTE: contenteditable has no undo model in bro — composition records no
// undo entries (unlike the controls' single discrete entry).

const SDLK_TAB = 9;

const root = document.getElementById('root');

function freshHost(html) {
    root.innerHTML =
        '<div id="ed" contenteditable="true" style="width:320px;height:60px;' +
        'border:1px solid #888;font-size:16px">' + (html || '') + '</div>';
    flush();
    return document.getElementById('ed');
}

function clickIntoText(el, dx) {
    const r = el.getBoundingClientRect();
    click(r.left + (dx === undefined ? 20 : dx), r.top + 12);
    flush();
}

function recordEvents(el, seq) {
    el.addEventListener('compositionstart', e => seq.push('cs:' + e.data));
    el.addEventListener('compositionupdate', e => seq.push('cu:' + e.data));
    el.addEventListener('compositionend', e => seq.push('ce:' + e.data));
    el.addEventListener('input', e =>
        seq.push('in:' + e.inputType + ':' + (e.data === null ? '' : e.data) +
                 ':' + (e.isComposing ? 1 : 0)));
}

// --- Full compose → update → commit with event-order recording --------------
{
    const ed = freshHost('hello');
    clickIntoText(ed, 3);                 // caret near "h|ello" area
    const sel = window.getSelection();
    sel.collapse(ed.firstChild, 2);       // deterministic: "he|llo"
    const seq = [];
    recordEvents(ed, seq);

    imeCompose('に');
    assert(ed.textContent === 'heにllo',
           'preedit visible in textContent, got: ' + JSON.stringify(ed.textContent));
    imeCompose('にほ');
    assert(ed.textContent === 'heにほllo',
           'updated preedit replaces the old one, got: ' + JSON.stringify(ed.textContent));
    imeCommit('日本');
    assert(ed.textContent === 'he日本llo',
           'commit replaces preedit with committed text, got: ' + JSON.stringify(ed.textContent));
    assert(ed.childNodes.length === 1 && ed.firstChild.nodeType === 3,
           'commit leaves one coherent text node, got ' + ed.childNodes.length + ' children');

    const expected = [
        'cs:',                                        // no selection replaced
        'cu:に', 'in:insertCompositionText:に:1',
        'cu:にほ', 'in:insertCompositionText:にほ:1',
        'cu:日本', 'in:insertCompositionText:日本:1',
        'ce:日本',
    ].join(',');
    assert(seq.join(',') === expected,
           'event order start→update/input pairs→final update/input→end on the HOST, got: ' + seq.join(','));
}

// --- Cancel restores the pre-composition DOM exactly ------------------------
{
    const ed = freshHost('abcd');
    clickIntoText(ed);
    window.getSelection().collapse(ed.firstChild, 2);
    const nodeBefore = ed.firstChild;
    const seq = [];
    recordEvents(ed, seq);

    imeCompose('か');
    assert(ed.textContent === 'abかcd', 'preedit spliced at caret, got: ' + JSON.stringify(ed.textContent));
    imeCancel();
    assert(ed.textContent === 'abcd',
           'cancel restores the pre-composition text, got: ' + JSON.stringify(ed.textContent));
    assert(ed.childNodes.length === 1 && ed.firstChild === nodeBefore,
           'cancel keeps the original text node (no churn)');
    assert(seq.join(',').endsWith('cu:,in:insertCompositionText::1,ce:'),
           'cancel order update("")→input→end(""), got: ' + seq.join(','));
}

// --- Empty host: preedit creates a text node, cancel removes it again -------
{
    const ed = freshHost('');
    clickIntoText(ed);
    // Nothing to hit-test into — seat the caret in the empty host directly.
    window.getSelection().collapse(ed, 0);
    imeCompose('ほ');
    assert(ed.textContent === 'ほ',
           'preedit in a created text node, got: ' + JSON.stringify(ed.textContent));
    assert(ed.childNodes.length === 1, 'one created text node during composition');
    imeCancel();
    assert(ed.textContent === '' && ed.childNodes.length === 0,
           'cancel removes the created node — DOM back to empty, got ' +
           ed.childNodes.length + ' children');

    // And a commit into the empty host keeps the created node with the text.
    window.getSelection().collapse(ed, 0);
    imeCompose('せ');
    imeCommit('世界');
    assert(ed.textContent === '世界' && ed.childNodes.length === 1,
           'commit keeps the created node, got: ' + JSON.stringify(ed.textContent));
}

// --- Element boundary: caret after <b>bold</b> ------------------------------
{
    const ed = freshHost('<b>bold</b>');
    clickIntoText(ed);
    const b = ed.querySelector('b');
    window.getSelection().collapse(ed, 1);   // host-level caret after the <b>
    imeCompose('に');
    assert(ed.textContent === 'boldに',
           'preedit lands after the bold run, got: ' + JSON.stringify(ed.textContent));
    assert(b.textContent === 'bold', 'the <b> subtree is untouched');
    assert(ed.childNodes.length === 2 && ed.childNodes[1].nodeType === 3,
           'preedit text node inserted at the boundary (B + text)');
    imeCommit('日');
    assert(ed.textContent === 'bold日' && ed.childNodes.length === 2,
           'committed at the boundary, got: ' + JSON.stringify(ed.textContent));
}

// --- Composing over a selection replaces it (deletion sticks on cancel) -----
{
    const ed = freshHost('hello world');
    clickIntoText(ed);
    const tn = ed.firstChild;
    window.getSelection().setBaseAndExtent(tn, 6, tn, 11);   // "world"
    let csData = null;
    ed.addEventListener('compositionstart', e => { csData = e.data; });
    imeCompose('せ');
    assert(csData === 'world', 'compositionstart.data is the replaced selection, got: ' + csData);
    assert(ed.textContent === 'hello せ',
           'selection replaced by preedit, got: ' + JSON.stringify(ed.textContent));
    imeCommit('世界');
    assert(ed.textContent === 'hello 世界',
           'committed over the selection, got: ' + JSON.stringify(ed.textContent));
}

// --- Non-ASCII / astral preedit round-trips exactly -------------------------
{
    const ed = freshHost('x');
    clickIntoText(ed);
    window.getSelection().collapse(ed.firstChild, 1);
    const seen = [];
    ed.addEventListener('compositionupdate', e => seen.push(e.data));
    imeCompose('🀄');                       // astral (surrogate pair in JS)
    assert(ed.textContent === 'x🀄',
           'astral preedit intact, got: ' + JSON.stringify(ed.textContent));
    imeCompose('🀄𝄞');
    imeCommit('🀄𝄞😀');
    assert(ed.textContent === 'x🀄𝄞😀',
           'astral commit intact (no byte-y leaks), got: ' + JSON.stringify(ed.textContent));
    assert(seen.join('|') === '🀄|🀄𝄞|🀄𝄞😀',
           'composition event data are exact JS strings, got: ' + seen.join('|'));
    // Follow-up plain typing still lands after the committed run.
    textInput('!');
    assert(ed.textContent === 'x🀄𝄞😀!',
           'typing after commit appends at the caret, got: ' + JSON.stringify(ed.textContent));
}

// --- Never strand: click elsewhere commits ----------------------------------
{
    root.innerHTML =
        '<div id="ed" contenteditable="true" style="width:200px;height:40px;' +
        'border:1px solid #888">seed</div>' +
        '<div id="other" style="width:200px;height:40px">elsewhere</div>';
    flush();
    const ed = document.getElementById('ed');
    clickIntoText(ed);
    window.getSelection().collapse(ed.firstChild, 4);
    let ended = false;
    ed.addEventListener('compositionend', () => { ended = true; });
    imeCompose('き');
    const ro = document.getElementById('other').getBoundingClientRect();
    click(ro.left + 10, ro.top + 10);
    flush();
    assert(ed.textContent === 'seedき',
           'mouse press elsewhere commits the preedit, got: ' + JSON.stringify(ed.textContent));
    assert(ended, 'compositionend fired on the click-away commit');
}

// --- Never strand: Tab commits ----------------------------------------------
{
    root.innerHTML =
        '<div id="ed" contenteditable="true" style="width:200px;height:40px;' +
        'border:1px solid #888">ab</div><input id="i1" type="text">';
    flush();
    const ed = document.getElementById('ed');
    clickIntoText(ed);
    window.getSelection().collapse(ed.firstChild, 2);
    let ended = false;
    ed.addEventListener('compositionend', () => { ended = true; });
    imeCompose('た');
    keyDown(SDLK_TAB, 0, 0);
    keyUp(SDLK_TAB, 0, 0);
    flush();
    assert(ed.textContent === 'abた',
           'Tab commits the preedit, got: ' + JSON.stringify(ed.textContent));
    assert(ended, 'compositionend fired on the Tab commit');
}

// --- Never strand: programmatic .focus() elsewhere commits -------------------
{
    root.innerHTML =
        '<div id="ed" contenteditable="true" style="width:200px;height:40px;' +
        'border:1px solid #888">xy</div><input id="i2" type="text">';
    flush();
    const ed = document.getElementById('ed');
    clickIntoText(ed);
    window.getSelection().collapse(ed.firstChild, 2);
    let ended = false;
    ed.addEventListener('compositionend', () => { ended = true; });
    imeCompose('ま');
    document.getElementById('i2').focus();
    flush();
    assert(ed.textContent === 'xyま',
           'programmatic focus() elsewhere commits the preedit, got: ' + JSON.stringify(ed.textContent));
    assert(ended, 'compositionend fired on the focus() commit');
}

// --- Script rewrites the text mid-composition: stale composition dropped ----
{
    const ed = freshHost('abc');
    clickIntoText(ed);
    window.getSelection().collapse(ed.firstChild, 3);
    imeCompose('か');
    ed.textContent = 'rewritten';          // rewrites the preedit's node
    flush();
    let seq = [];
    recordEvents(ed, seq);
    imeCommit('X');
    // The stale composition was dropped (its bytes are gone); like the
    // controls after a script .value write, the commit degrades to a plain
    // insert at the caret — crucially it must NOT splice over script-owned
    // text, and no composition events fire.
    assert(ed.textContent.indexOf('X') !== -1 &&
           ed.textContent.replace('X', '') === 'rewritten',
           'script text kept, commit degraded to plain insert, got: ' + JSON.stringify(ed.textContent));
    assert(seq.join(',') === 'in:insertText:X:0',
           'no composition events for the degraded commit, got: ' + seq.join(','));
}

// --- imeCommit without a composition falls through to plain insert ----------
{
    const ed = freshHost('ab');
    clickIntoText(ed);
    window.getSelection().collapse(ed.firstChild, 2);
    const seq = [];
    recordEvents(ed, seq);
    imeCommit('あ');
    assert(ed.textContent === 'abあ',
           'bare commit inserts like typing, got: ' + JSON.stringify(ed.textContent));
    assert(seq.join(',') === 'in:insertText:あ:0',
           'bare commit is a plain insertText (no composition events), got: ' + seq.join(','));
}
