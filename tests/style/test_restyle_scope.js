// What a child-list change and a focus change re-match, and that it is still
// right (src/dom/document_style.cpp resolveStylesRecursive, setActiveElement).
//
// A view put back where it was (the same parent, place and siblings) re-matches
// only its root: what is inside it matches what it matched. It re-matches its
// subtree when it moved under another parent, or when its place changed and a
// selector reads places in a qualifier (`li:first-child .x`, `.a + .b .x`).
// Focus moving re-matches the element and, below it, only what a rule naming
// a focus pseudo-class in a qualifier could make its subject; :focus-within
// re-matches the ancestors.

const root = document.getElementById('root');
const style = document.createElement('style');
style.textContent = `
  .rs-host .rs-x { color: rgb(1, 1, 1); }
  .rs-other .rs-x { color: rgb(2, 2, 2); }
  .rs-list li:first-child .rs-x { color: rgb(3, 3, 3); }
  .rs-a + .rs-b .rs-y { color: rgb(4, 4, 4); }
  .rs-empty:empty { color: rgb(5, 5, 5); }
  .rs-vl:focus .rs-row.rs-cur { color: rgb(6, 6, 6); }
  .rs-wrap:focus-within { color: rgb(7, 7, 7); }
`;
document.head.appendChild(style);
flush();

const eq = (got, want, msg) => assert(got === want, `${msg}: got ${got}, want ${want}`);
const color = (el) => getComputedStyle(el).getPropertyValue('color').replace(/\s+/g, '');
const el = (tag, cls, ...kids) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    for (const k of kids) e.append(k);
    return e;
};
const view = (n) => {
    const v = el('div', 'rs-view');
    for (let i = 0; i < n; i++) v.append(el('div', 'rs-item', el('span', 'rs-x', 'x' + i)));
    return v;
};

// ---- a view put back where it was ----------------------------------------------------
{
    const host = el('div', 'rs-host');
    root.append(host);
    const a = view(30), b = view(30);
    host.append(a);
    flush();
    eq(color(a.querySelector('.rs-x')), 'rgb(1,1,1)', 'styled under the host');
    host.replaceChildren(b);
    flush();
    perf.reset();
    host.replaceChildren(a);
    flush();
    const p = perf.stats();
    assert(p.elementsStyled <= 4, `putting a 61-element view back re-matches its root, not its subtree (${p.elementsStyled} styled)`);
    eq(color(a.querySelector('.rs-x')), 'rgb(1,1,1)', 'and its styles are right');

    // Moved under another parent: everything in it re-matches.
    const other = el('div', 'rs-other');
    root.append(other);
    other.append(a);
    flush();
    eq(color(a.querySelector('.rs-x')), 'rgb(2,2,2)', 'moved under another parent: re-matched');
    host.remove(); other.remove(); flush();
}

// ---- a place read in a qualifier --------------------------------------------------------
{
    const ul = el('ul', 'rs-list');
    const li = (t) => el('li', '', el('span', 'rs-x', t));
    const l1 = li('one'), l2 = li('two');
    ul.append(l1, l2);
    root.append(ul);
    flush();
    eq(color(l1.querySelector('.rs-x')), 'rgb(3,3,3)', 'li:first-child .x');
    eq(color(l2.querySelector('.rs-x')) !== 'rgb(3,3,3)', true, 'not the second');
    ul.insertBefore(l2, l1);
    flush();
    eq(color(l2.querySelector('.rs-x')), 'rgb(3,3,3)', 'reordered: the new first child\'s .x');
    eq(color(l1.querySelector('.rs-x')) !== 'rgb(3,3,3)', true, 'and not the old one\'s');
    ul.prepend(li('zero'));
    flush();
    eq(color(l2.querySelector('.rs-x')) !== 'rgb(3,3,3)', true, 'one inserted before it: no longer first');
    ul.remove(); flush();

    const wrap = el('div', '');
    const a = el('div', 'rs-a'), b = el('div', 'rs-b', el('span', 'rs-y', 'y'));
    wrap.append(b);
    root.append(wrap);
    flush();
    eq(color(b.firstChild) !== 'rgb(4,4,4)', true, '.a + .b .y: no .a yet');
    wrap.insertBefore(a, b);
    flush();
    eq(color(b.firstChild), 'rgb(4,4,4)', 'a sibling inserted before it reaches the subtree');
    a.remove();
    flush();
    eq(color(b.firstChild) !== 'rgb(4,4,4)', true, 'and removed again');
    wrap.remove(); flush();
}

// ---- :empty follows the child list -------------------------------------------------------
{
    const box = el('div', 'rs-empty');
    root.append(box);
    flush();
    eq(color(box), 'rgb(5,5,5)', ':empty');
    const kid = el('span', '', 'k');
    box.append(kid);
    flush();
    eq(color(box) !== 'rgb(5,5,5)', true, 'not empty once it has a child');
    kid.remove();
    flush();
    eq(color(box), 'rgb(5,5,5)', 'empty again');
    box.remove(); flush();
}

// ---- focus -----------------------------------------------------------------------------
{
    const vl = el('div', 'rs-vl');
    vl.tabIndex = 0;
    const rows = [];
    for (let i = 0; i < 40; i++) rows.push(el('div', 'rs-row' + (i === 7 ? ' rs-cur' : ''), 'r' + i));
    vl.append(...rows);
    const outside = el('button', '', 'b');
    root.append(vl, outside);
    flush();
    eq(color(rows[7]) !== 'rgb(6,6,6)', true, 'not focused: the cursor row plain');
    perf.reset();
    vl.focus();
    flush();
    const p = perf.stats();
    eq(color(rows[7]), 'rgb(6,6,6)', '.vl:focus .row.cur: the cursor row');
    eq(color(rows[8]) !== 'rgb(6,6,6)', true, 'not another row');
    assert(p.elementsStyled <= 6, `focusing a 40-row list re-matches it and its cursor row, not every row (${p.elementsStyled} styled)`);
    outside.focus();
    flush();
    eq(color(rows[7]) !== 'rgb(6,6,6)', true, 'focus gone: plain again');

    const wrapF = el('div', 'rs-wrap');
    const input = el('input', '');
    wrapF.append(el('div', '', input));
    root.append(wrapF);
    flush();
    eq(color(wrapF) !== 'rgb(7,7,7)', true, ':focus-within: not yet');
    input.focus();
    flush();
    eq(color(wrapF), 'rgb(7,7,7)', ':focus-within reaches the ancestor');
    outside.focus();
    flush();
    eq(color(wrapF) !== 'rgb(7,7,7)', true, 'and leaves it');
    vl.remove(); outside.remove(); wrapF.remove(); flush();
}
style.remove();
flush();
