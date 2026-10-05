// <terminal>: search over the screen and history (bropty's search: literal
// or regex on brosearch, smart / sensitive / insensitive case, whole word).
// Matching runs on the session's thread, history in slices, so `searchStatus`
// reports progress and `searchchange` fires as it moves; the search stays
// current as output arrives. bro_terminal_test checks the same matches
// against bropty's own Search over the same text.

function waitFor(pred, what, ms) {
    const end = Date.now() + (ms || 15000);
    while (Date.now() < end) {
        advanceTime(16);
        if (pred()) return true;
        wallSleep(5);
    }
    assert(pred(), 'timed out waiting for ' + what);
    return false;
}

if (!bro.terminal || !bro.terminal.available) {
    skipTest('<terminal> is compiled out of this build (BRO_WITH_TERMINAL)');
} else {
    const t = document.createElement('terminal');
    t.setAttribute('cols', '40');
    t.setAttribute('rows', '5');
    document.body.appendChild(t);
    flush();
    advanceTime(16);
    const changes = [];
    t.addEventListener('searchchange', (e) => changes.push(e.detail));

    let text = 'foo bar\r\nFoo baz\r\nfoobar qux\r\n';
    for (let i = 0; i < 30; ++i) text += 'filler ' + i + '\r\n';
    t.feed(text + 'foo end');
    waitFor(() => t.frameText().includes('foo end'), 'output shown');

    const count = (pattern, opts) => {
        t.search(pattern, opts);
        waitFor(() => t.searchStatus.complete, 'search "' + pattern + '" complete');
        return t.searchStatus.count;
    };

    assert(count('foo') === 4, 'smart case, lower-case pattern: insensitive (4), got ' + t.searchStatus.count);
    assert(count('Foo') === 1, 'smart case, a capital: sensitive (1), got ' + t.searchStatus.count);
    assert(count('foo', { caseMode: 'sensitive' }) === 3, 'sensitive (3), got ' + t.searchStatus.count);
    assert(count('FOO', { caseMode: 'insensitive' }) === 4, 'insensitive (4), got ' + t.searchStatus.count);
    assert(count('foo', { wholeWord: true }) === 3, 'whole word (3), got ' + t.searchStatus.count);
    assert(count('ba[rz]', { regex: true }) === 3, 'regex (3), got ' + t.searchStatus.count);
    assert(count('ba[rz]') === 0, 'the same text literally (0), got ' + t.searchStatus.count);
    assert(count('filler \\d+', { regex: true }) === 30, 'regex over history (30), got ' + t.searchStatus.count);

    let threw = null;
    try { t.search('(', { regex: true }); } catch (e) { threw = e; }
    assert(threw && threw.name === 'SyntaxError', 'a bad regex throws SyntaxError: ' + threw);
    threw = null;
    try { t.search('x', { caseMode: 'loud' }); } catch (e) { threw = e; }
    assert(threw instanceof TypeError, 'a bad caseMode throws TypeError');

    // Next / previous: the current match moves (wrapping) and is scrolled to.
    count('foo');
    const first = t.searchNext();
    assert(first && t.searchStatus.current !== null, 'searchNext gives a range: ' + JSON.stringify(first));
    const second = t.searchNext();
    assert(second && (second.startRow !== first.startRow || second.startCol !== first.startCol), 'the next one');
    const back = t.searchPrevious();
    assert(back && back.startRow === first.startRow && back.startCol === first.startCol, 'and back');
    assert(t.textInRange(back).toLowerCase() === 'foo', 'the match reads "foo": ' + t.textInRange(back));
    t.scrollToTop();
    advanceTime(16);
    const firstRow = t.viewport.firstRow;
    const top = t.searchNext();
    assert(top !== null, 'searchNext from the top');
    waitFor(() => t.viewport.topRow <= top.startRow && top.startRow < t.viewport.topRow + 5,
            'the current match is brought into view');
    assert(firstRow === t.viewport.firstRow, 'history unchanged');

    // Output keeps the search current.
    t.feed('\r\nmore foo here');
    waitFor(() => t.searchStatus.count === 5, 'new output matched: ' + t.searchStatus.count);
    waitFor(() => changes.length > 0 && changes[changes.length - 1].count === 5, 'searchchange reports it');
    assert(changes[changes.length - 1].active === true, 'searchchange detail: ' + JSON.stringify(changes[changes.length - 1]));

    t.clearSearch();
    waitFor(() => !t.searchStatus.active, 'cleared');
    waitFor(() => changes[changes.length - 1].active === false, 'searchchange on clear');
}
