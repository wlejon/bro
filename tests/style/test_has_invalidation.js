// :has() reads below (or beside) its element, so a change there must re-match
// upward: the ancestors (and earlier siblings) a :has() rule can be about, and
// as far below or beside them as the rule's combinators reach
// (Document::invalidateHasAround, Cascade::hasReach). And a sheet with a
// :has() rule in it must not make every class or hover change re-match whole
// subtrees: only the elements a :has() compound can name are involved.

const root = document.getElementById('root');
const style = document.createElement('style');
style.textContent = `
  .hi-card:has(.hi-badge) { color: rgb(1, 1, 1); }
  .hi-list:has(.hi-item.sel) .hi-title { color: rgb(2, 2, 2); }
  .hi-head:has(.hi-glyph) + .hi-body { padding-left: 30px; }
  .hi-a:has(+ .hi-b) { color: rgb(3, 3, 3); }
  .hi-form:has(input:focus) { color: rgb(4, 4, 4); }
  .hi-box:has([data-on]) { color: rgb(5, 5, 5); }
  .hi-plain.on { background-color: rgb(6, 6, 6); }
  .hi-menu:has(.hi-entry:hover) .hi-label { color: rgb(8, 8, 8); }
`;
document.head.appendChild(style);
flush();

const eq = (got, want, msg) => assert(got === want, `${msg}: got ${got}, want ${want}`);
const prop = (el, p) => getComputedStyle(el).getPropertyValue(p).replace(/\s+/g, '');
const el = (tag, cls, ...kids) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    for (const k of kids) e.append(k);
    return e;
};
const not = (el, p, v, msg) => assert(prop(el, p) !== v, `${msg}: still ${v}`);

// ---- a child added and removed ------------------------------------------------------
{
    const card = el('div', 'hi-card', el('div', '', el('span', '', 'deep')));
    root.append(card);
    flush();
    not(card, 'color', 'rgb(1,1,1)', 'no badge yet');
    const badge = el('span', 'hi-badge', '!');
    card.firstChild.append(badge);
    flush();
    eq(prop(card, 'color'), 'rgb(1,1,1)', 'a badge added deep inside: .card:has(.badge)');
    badge.remove();
    flush();
    not(card, 'color', 'rgb(1,1,1)', 'the badge removed');
    card.remove(); flush();
}

// ---- a class toggled on a descendant, read in a qualifier --------------------------------
{
    const item = el('div', 'hi-item', 'i');
    const title = el('div', 'hi-title', 't');
    const list = el('div', 'hi-list', title, el('div', '', el('div', '', item)));
    root.append(list);
    flush();
    not(title, 'color', 'rgb(2,2,2)', 'nothing selected');
    item.classList.add('sel');
    flush();
    eq(prop(title, 'color'), 'rgb(2,2,2)', 'a class on a deep item: .list:has(.item.sel) .title');
    item.classList.remove('sel');
    flush();
    not(title, 'color', 'rgb(2,2,2)', 'the class removed');
    list.remove(); flush();
}

// ---- through a sibling combinator (the helm kit's dialog rule) ----------------------------
{
    const head = el('div', 'hi-head', 'h');
    const body = el('div', 'hi-body', 'b');
    const wrap = el('div', '', head, body);
    root.append(wrap);
    flush();
    not(body, 'padding-left', '30px', 'no glyph');
    head.append(el('span', 'hi-glyph', 'g'));
    flush();
    eq(prop(body, 'padding-left'), '30px', '.head:has(.glyph) + .body');
    head.lastChild.remove();
    flush();
    not(body, 'padding-left', '30px', 'the glyph removed');
    wrap.remove(); flush();
}

// ---- an argument that reads a sibling ---------------------------------------------------
{
    const a = el('div', 'hi-a', 'a');
    const wrap = el('div', '', a);
    root.append(wrap);
    flush();
    not(a, 'color', 'rgb(3,3,3)', 'no .b after it');
    const b = el('div', 'hi-b', 'b');
    wrap.append(b);
    flush();
    eq(prop(a, 'color'), 'rgb(3,3,3)', '.a:has(+ .b): a sibling inserted after it');
    b.className = '';
    flush();
    not(a, 'color', 'rgb(3,3,3)', 'the sibling\'s class removed');
    wrap.remove(); flush();
}

// ---- an attribute -----------------------------------------------------------------------
{
    const inner = el('span', '', 'x');
    const box = el('div', 'hi-box', el('div', '', inner));
    root.append(box);
    flush();
    not(box, 'color', 'rgb(5,5,5)', 'no attribute');
    inner.setAttribute('data-on', '1');
    flush();
    eq(prop(box, 'color'), 'rgb(5,5,5)', '.box:has([data-on])');
    inner.removeAttribute('data-on');
    flush();
    not(box, 'color', 'rgb(5,5,5)', 'the attribute removed');
    box.remove(); flush();
}

// ---- focus inside -----------------------------------------------------------------------
{
    const input = el('input', '');
    const form = el('div', 'hi-form', el('div', '', input));
    const out = el('button', '', 'o');
    root.append(form, out);
    flush();
    not(form, 'color', 'rgb(4,4,4)', 'nothing focused');
    input.focus();
    flush();
    eq(prop(form, 'color'), 'rgb(4,4,4)', '.form:has(input:focus)');
    out.focus();
    flush();
    not(form, 'color', 'rgb(4,4,4)', 'focus moved out');
    form.remove(); out.remove(); flush();
}

// ---- hover inside ------------------------------------------------------------------------
{
    const entry = el('div', 'hi-entry', 'e');
    entry.style.cssText = 'width:100px;height:20px';
    const label = el('div', 'hi-label', 'l');
    const menu = el('div', 'hi-menu', label, el('div', '', entry));
    menu.style.cssText = 'position:absolute;left:0;top:0';
    root.append(menu);
    flush();
    mouseMove(500, 500);
    flush();
    not(label, 'color', 'rgb(8,8,8)', 'not hovered');
    const r = entry.getBoundingClientRect();
    mouseMove(r.left + 5, r.top + 5);
    flush();
    eq(prop(label, 'color'), 'rgb(8,8,8)', '.menu:has(.entry:hover) .label');
    mouseMove(500, 500);
    flush();
    not(label, 'color', 'rgb(8,8,8)', 'the pointer left');
    menu.remove(); flush();
}

// ---- and a :has() in the sheet does not widen unrelated changes ---------------------------
{
    const big = el('div', '');
    for (let i = 0; i < 60; i++) big.append(el('div', 'hi-row', el('span', '', 'r' + i)));
    const plain = el('div', 'hi-plain', big);
    root.append(plain);
    flush();
    perf.reset();
    plain.classList.add('on');
    flush();
    const p = perf.stats();
    eq(prop(plain, 'background-color'), 'rgb(6,6,6)', 'a subject-only class (not inherited)');
    assert(p.elementsStyled <= 70, `with :has() rules in the sheet, a class only a subject names re-matches the element, ` +
        `not its 121-element subtree (${p.elementsStyled} styled)`);
    plain.remove(); flush();
}
style.remove();
flush();
