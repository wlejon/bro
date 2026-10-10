// Style sharing (src/dom/style_share.h): an element whose style inputs are
// unchanged keeps its style, and one whose inputs equal another's copies that
// one's style. Both are only right if EVERY input that could change the result
// is in the key. Each case below builds two elements (or one element twice)
// that agree on everything except one input, and checks the style follows
// that input — the failure mode of a too-small key is a style copied from the
// wrong element, or kept from before the change.

const root = document.getElementById('root');

const sheet = document.createElement('style');
sheet.textContent = `
  #ss { font-size: 16px; }
  .ss-item { color: rgb(1, 1, 1); width: 10px; }
  .ss-item[data-x="on"] { color: rgb(2, 2, 2); }
  .ss-item:hover { color: rgb(3, 3, 3); }
  .ss-item:focus { color: rgb(4, 4, 4); }
  .ss-item:nth-child(3) { width: 33px; }
  .ss-item + .ss-item.after { width: 44px; }
  .ss-dark .ss-item { background-color: rgb(5, 5, 5); }
  .ss-var { --tone: rgb(6, 6, 6); }
  .ss-item.uses-var { border-left: 1px solid var(--tone, rgb(7, 7, 7)); }
  .ss-item.rem { font-size: 2rem; }
  .ss-item.em { height: 2em; }
  .ss-item.scheme { color-scheme: light dark; background-color: light-dark(rgb(8, 8, 8), rgb(9, 9, 9)); }
`;
document.head.appendChild(sheet);

const box = document.createElement('div');
box.id = 'ss';
root.appendChild(box);

const eq = (got, want, msg) => assert(got === want, `${msg}: got ${got}, want ${want}`);
const prevScheme = bro.settings.get('appearance.colorScheme');
const setColorScheme = (s) => bro.settings.set('appearance.colorScheme', s);
const cs = (el, p) => getComputedStyle(el).getPropertyValue(p).replace(/\s+/g, '');
const item = (cls = '') => {
    const el = document.createElement('div');
    el.className = `ss-item ${cls}`.trim();
    el.tabIndex = 0;
    el.textContent = 'x';
    return el;
};
const row = (n, cls = '') => {
    const r = document.createElement('div');
    for (let i = 0; i < n; i++) r.appendChild(item(cls));
    return r;
};

// ---- the counters say the caches are in use at all ----------------------------
{
    const r = row(10);
    box.appendChild(r);
    perf.reset();
    flush();
    const p = perf.stats();
    assert(p.stylesShared >= 5, `ten identical siblings share styles (${p.stylesShared} shared, ${p.stylesResolved} resolved)`);
    r.remove();
    flush();
    perf.reset();
    box.appendChild(r);
    flush();
    const q = perf.stats();
    assert(q.stylesKept >= 10, `put back unchanged, they keep their styles (${q.stylesKept} kept)`);
    eq(cs(r.children[0], 'color'), 'rgb(1,1,1)', 'and the style is right');
    r.remove();
    flush();
}

// ---- an attribute a selector reads -----------------------------------------------
{
    const r = row(4);
    r.children[1].dataset.x = 'on';
    box.appendChild(r);
    flush();
    eq(cs(r.children[0], 'color'), 'rgb(1,1,1)', 'no data-x: base colour');
    eq(cs(r.children[1], 'color'), 'rgb(2,2,2)', 'data-x=on: not shared with its sibling');
    eq(cs(r.children[2], 'color'), 'rgb(1,1,1)', 'the next sibling is not given the data-x style');
    // Changing it on a kept element.
    r.remove(); flush();
    r.children[2].dataset.x = 'on';
    box.appendChild(r);
    flush();
    eq(cs(r.children[2], 'color'), 'rgb(2,2,2)', 'set while detached: not kept from before');
    r.remove(); flush();
}

// ---- :hover and :focus ------------------------------------------------------------
{
    const r = row(4);
    box.appendChild(r);
    flush();
    const b = r.children[1].getBoundingClientRect();
    mouseMove(b.left + 2, b.top + 2);
    flush();
    eq(cs(r.children[1], 'color'), 'rgb(3,3,3)', ':hover applies to the hovered one');
    eq(cs(r.children[0], 'color'), 'rgb(1,1,1)', 'and not to its sibling');
    mouseMove(1000, 700);
    flush();
    eq(cs(r.children[1], 'color'), 'rgb(1,1,1)', ':hover gone');
    r.children[2].focus();
    flush();
    eq(cs(r.children[2], 'color'), 'rgb(4,4,4)', ':focus applies');
    eq(cs(r.children[3], 'color'), 'rgb(1,1,1)', 'not to the next sibling');
    r.children[2].blur();
    flush();
    eq(cs(r.children[2], 'color'), 'rgb(1,1,1)', ':focus gone');
    r.remove(); flush();
}

// ---- position: :nth-child and a sibling combinator ----------------------------------
{
    const r = row(5);
    box.appendChild(r);
    flush();
    eq(cs(r.children[2], 'width'), '33px', 'the third child matches :nth-child(3)');
    eq(cs(r.children[3], 'width'), '10px', 'the fourth does not');
    // Put a new first child in: every position moves, with no attribute changed.
    r.remove(); flush();
    r.insertBefore(item(), r.firstChild);
    box.appendChild(r);
    flush();
    eq(cs(r.children[2], 'width'), '33px', 'the new third child matches');
    eq(cs(r.children[3], 'width'), '10px', 'the old third one (now fourth) is not kept at 33px');
    // `.ss-item + .ss-item.after` reads the previous sibling.
    const s = document.createElement('div');
    const lone = item('after');
    s.appendChild(lone);
    box.appendChild(s);
    flush();
    eq(cs(lone, 'width'), '10px', 'a first child with no previous sibling');
    s.remove(); flush();
    s.insertBefore(item(), lone);
    box.appendChild(s);
    flush();
    eq(cs(lone, 'width'), '44px', 'given a previous sibling while detached: re-matched, not kept');
    r.remove(); s.remove(); flush();
}

// ---- inline style -----------------------------------------------------------------
{
    const r = row(3);
    r.children[1].style.width = '77px';
    box.appendChild(r);
    flush();
    eq(cs(r.children[1], 'width'), '77px', 'inline style applies');
    eq(cs(r.children[0], 'width'), '10px', 'and is not copied to a sibling');
    r.remove(); flush();
    r.children[1].style.width = '78px';
    box.appendChild(r);
    flush();
    eq(cs(r.children[1], 'width'), '78px', 'changed while detached: not kept');
    r.remove(); flush();
}

// ---- the parent: inherited values, ancestors' classes, custom properties ----------------
{
    const a = row(2);
    const b = row(2);
    b.style.color = 'rgb(20, 20, 20)';
    const c = row(2, 'uses-var');
    const d = row(2, 'uses-var');
    d.className = 'ss-var';
    box.append(a, b, c, d);
    flush();
    eq(cs(a.children[0].appendChild(document.createElement('span')), 'color'), 'rgb(1,1,1)', 'a span inherits the item colour');
    // Spans under items in differently-coloured parents: the items themselves
    // set color, so their own style is the same; give the spans a parent that
    // differs only in an inherited value instead.
    const p1 = document.createElement('div');
    const p2 = document.createElement('div');
    p2.style.fontSize = '20px';
    p1.appendChild(item('em'));
    p2.appendChild(item('em'));
    box.append(p1, p2);
    flush();
    eq(cs(p1.firstChild, 'height'), '32px', '2em under a 16px parent');
    eq(cs(p2.firstChild, 'height'), '40px', '2em under a 20px parent: not shared');
    // Move an element to a parent with a different inherited value.
    const moved = p1.firstChild;
    p2.appendChild(moved);
    flush();
    eq(cs(moved, 'height'), '40px', 'moved under the 20px parent: re-resolved, not kept');
    // An ancestor class that only a descendant selector reads.
    a.remove(); flush();
    a.className = 'ss-dark';
    box.appendChild(a);
    flush();
    eq(cs(a.children[1], 'background-color'), 'rgb(5,5,5)', 'an ancestor class picked up while detached');
    // Custom properties.
    eq(cs(c.children[0], 'border-left-color'), 'rgb(7,7,7)', 'var() fallback with no --tone');
    eq(cs(d.children[0], 'border-left-color'), 'rgb(6,6,6)', '--tone from the parent: not shared with the fallback one');
    c.remove(); flush();
    c.className = 'ss-var';
    box.appendChild(c);
    flush();
    eq(cs(c.children[1], 'border-left-color'), 'rgb(6,6,6)', '--tone arriving while detached reaches the kept children');
    a.remove(); b.remove(); c.remove(); d.remove(); p1.remove(); p2.remove(); flush();
}

// ---- document-wide inputs: rem, the colour scheme ------------------------------------
{
    const r = row(2, 'rem');
    box.appendChild(r);
    flush();
    const html = document.documentElement;
    // font-size is the one length bro resolves to px at computed-value time,
    // rem included, so it is the one that reads the root's font size.
    eq(cs(r.children[0], 'font-size'), '32px', '2rem under a 16px root');
    html.style.fontSize = '20px';
    r.remove();
    flush();
    box.appendChild(r);
    flush();
    eq(cs(r.children[0], 'font-size'), '40px', '2rem follows the root font size');
    html.style.fontSize = '';
    flush();

    const s = row(2, 'scheme');
    box.appendChild(s);
    flush();
    setColorScheme('light');
    flush();
    const light = cs(s.children[0], 'background-color');
    s.remove();
    flush();
    setColorScheme('dark');
    box.appendChild(s);
    flush();
    const dark = cs(s.children[0], 'background-color');
    assert(light !== dark, `light-dark() follows the scheme while detached (${light} -> ${dark})`);
    eq(dark, 'rgb(9,9,9)', 'the dark branch');
    setColorScheme(prevScheme || 'light');
    flush();
    r.remove(); s.remove(); flush();
}

// ---- SVG presentation attributes -----------------------------------------------------
{
    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    const r1 = document.createElementNS('http://www.w3.org/2000/svg', 'rect');
    const r2 = document.createElementNS('http://www.w3.org/2000/svg', 'rect');
    r1.setAttribute('fill', 'rgb(10, 10, 10)');
    r2.setAttribute('fill', 'rgb(11, 11, 11)');
    svg.append(r1, r2);
    box.appendChild(svg);
    flush();
    eq(cs(r1, 'fill'), 'rgb(10,10,10)', 'fill attribute');
    eq(cs(r2, 'fill'), 'rgb(11,11,11)', 'a sibling with another fill attribute is not shared');
    svg.remove(); flush();
    r1.setAttribute('fill', 'rgb(12, 12, 12)');
    box.appendChild(svg);
    flush();
    eq(cs(r1, 'fill'), 'rgb(12,12,12)', 'changed while detached: not kept');
    svg.remove(); flush();
}

// ---- a sheet added later ------------------------------------------------------------
{
    const r = row(2);
    box.appendChild(r);
    flush();
    const late = document.createElement('style');
    late.textContent = '.ss-item.late { color: rgb(13, 13, 13); }';
    r.children[0].classList.add('late');
    document.head.appendChild(late);
    flush();
    eq(cs(r.children[0], 'color'), 'rgb(13,13,13)', 'a rule from a sheet added later');
    eq(cs(r.children[1], 'color'), 'rgb(1,1,1)', 'only where it matches');
    r.remove(); flush();
}
