// After a layout, a scroller whose content shrank has its offset brought back
// within range (engine::clampScrollOffsets). It looks only at the elements
// holding an offset, which the document keeps (Document::scrolledElements),
// rather than walking every element each frame; this checks it still finds
// every one: deep in a big document, inside a shadow root, back from
// display:none, after moving between documents; and that a big document's
// frame does not pay for its size.

const root = document.getElementById('root');
const eq = (got, want, msg) => assert(got === want, `${msg}: got ${got}, want ${want}`);

function scroller(h = 100, content = 1000) {
    const s = document.createElement('div');
    s.style.cssText = `height:${h}px;width:200px;overflow:auto`;
    const inner = document.createElement('div');
    inner.style.height = content + 'px';
    s.append(inner);
    return s;
}

// ---- deep in a big document, content shrinks: clamped, and scroll fires -------------------
{
    const big = document.createElement('div');
    for (let i = 0; i < 300; i++) {
        const row = document.createElement('div');
        for (let k = 0; k < 9; k++) row.append(document.createElement('span'));
        big.append(row);
    }
    let host = big;
    for (let i = 0; i < 10; i++) { const w = document.createElement('div'); w.append(host); host = w; }
    const s = scroller();
    big.lastChild.append(s);
    root.append(host);
    flush();
    s.scrollTop = 800;
    flush();
    eq(s.scrollTop, 800, 'scrolled to 800 of 900');
    let scrolls = 0;
    s.addEventListener('scroll', () => scrolls++);
    s.firstChild.style.height = '300px';
    flush();
    advanceTime(16);
    eq(s.scrollTop, 200, 'content shrank to 300: clamped to 200');
    assert(scrolls >= 1, 'and a scroll event fired: ' + scrolls);

    // A frame after a small change does not walk the document (~3000 elements).
    const t0 = perf.now();
    for (let i = 0; i < 200; i++) { big.children[i].className = i % 2 ? 'a' : 'b'; flush(); }
    const per = (perf.now() - t0) / 200;
    console.log(`  small change + flush in a ${root.querySelectorAll('*').length}-element document: ${per.toFixed(3)} ms`);
    assert(per < 1.5, `a small change's flush stays small: ${per.toFixed(3)} ms`);
    host.remove(); flush();
}

// ---- inside a shadow root -----------------------------------------------------------------
{
    const hostEl = document.createElement('div');
    const sr = hostEl.attachShadow({ mode: 'open' });
    const s = scroller();
    sr.append(s);
    root.append(hostEl);
    flush();
    s.scrollTop = 700;
    flush();
    eq(s.scrollTop, 700, 'a scroller in a shadow root scrolls');
    s.firstChild.style.height = '400px';
    flush();
    eq(s.scrollTop, 300, 'and is clamped when its content shrinks');
    hostEl.remove(); flush();
}

// ---- display:none and back: the offset is remembered, not clamped against nothing --------
{
    const wrap = document.createElement('div');
    const s = scroller();
    wrap.append(s);
    root.append(wrap);
    flush();
    s.scrollTop = 500;
    flush();
    wrap.style.display = 'none';
    flush();
    wrap.style.display = '';
    flush();
    eq(s.scrollTop, 500, 'an offset under display:none comes back as it was');
    wrap.remove(); flush();
}

// ---- detached: left alone until it is back ---------------------------------------------------
{
    const s = scroller();
    root.append(s);
    flush();
    s.scrollTop = 600;
    flush();
    s.remove();
    flush();
    s.firstChild.style.height = '200px';
    flush();
    root.append(s);
    flush();
    eq(s.scrollTop, 100, 'put back after its content shrank while detached: clamped then');
    s.remove(); flush();
}

// ---- moved to another document and back ---------------------------------------------------
{
    const s = scroller();
    root.append(s);
    flush();
    s.scrollTop = 650;
    flush();
    const other = document.implementation.createHTMLDocument('other');
    other.adoptNode(s);
    flush();
    document.adoptNode(s);
    s.firstChild.style.height = '250px';
    root.append(s);
    flush();
    eq(s.scrollTop, 150, 'adopted away and back: clamped in this document again');
    s.remove(); flush();
}
