// Content-sized grid tracks beside an fr track (Grid §12.4-12.8): an auto,
// max-content, min-content or fit-content() track is its content (base size
// the min-content contribution, grown toward max-content while free space is
// left), and the fr track takes the rest. `auto 1fr` on a 600px grid split it
// 300/300: the items were measured laid out at a guessed width (600 / 2),
// which a block-level item fills, and that width stood for their content.
// Two auto tracks still share what is left over (the stretch step), and
// fit-content() caps at its limit.

document.body.style.margin = '0';
const near = (a, b, tol = 1.5) => Math.abs(a - b) <= tol;

// The content's own width: a lone max-content box.
const probe = (html) => {
    const d = document.createElement('div');
    d.style.cssText = 'display:inline-block';
    d.innerHTML = html;
    document.body.appendChild(d);
    flush();
    const w = d.getBoundingClientRect().width;
    d.remove();
    return w;
};
const sem = probe('sem');
const bar = probe('bar');
const long = probe('a much longer label here');
assert(sem > 10 && sem < 60, `"sem" measures as text: ${sem}`);

function cols(template, firstHtml, secondHtml, extra = '') {
    const g = document.createElement('div');
    g.style.cssText = `display:grid; width:600px; grid-template-columns:${template}; ${extra}`;
    g.innerHTML = `<span>${firstHtml}</span><div>${secondHtml}</div>`;
    document.body.appendChild(g);
    flush();
    const r = [g.children[0].getBoundingClientRect().width, g.children[1].getBoundingClientRect().width];
    g.remove();
    return r;
}

for (const t of ['auto 1fr', 'max-content 1fr', 'min-content 1fr', 'fit-content(200px) 1fr']) {
    const [a, b] = cols(t, 'sem', 'bar');
    assert(near(a, sem), `${t}: the content track is its content (${sem}), got ${a}`);
    assert(near(b, 600 - sem), `${t}: the fr track takes the rest (${600 - sem}), got ${b}`);
}
{
    const [a, b] = cols('1fr auto', 'bar', 'sem');
    assert(near(b, sem) && near(a, 600 - sem), `1fr auto: ${a} / ${b}`);
}
{
    const [a, b] = cols('100px 1fr', 'sem', 'bar');
    assert(near(a, 100) && near(b, 500), `100px 1fr is unchanged: ${a} / ${b}`);
}
{
    // Two auto tracks: each its max-content and an equal share of the rest.
    const [a, b] = cols('auto auto', 'sem', 'bar');
    const share = (600 - sem - bar) / 2;
    assert(near(a, sem + share) && near(b, bar + share), `auto auto: ${a} / ${b} (want ${sem + share} / ${bar + share})`);
}
{
    // fit-content() caps at its limit; min-content wraps to the longest word.
    const [a] = cols('fit-content(60px) 1fr', 'a much longer label here', 'bar');
    assert(near(a, 60), `fit-content(60px) caps a long label at 60px, got ${a}`);
    const [m] = cols('min-content 1fr', 'a much longer label here', 'bar');
    assert(m < long - 20, `min-content is narrower than the label's max-content (${long}), got ${m}`);
    const [x] = cols('max-content 1fr', 'a much longer label here', 'bar');
    assert(near(x, long), `max-content is the label's max-content (${long}), got ${x}`);
}
{
    // justify-content: start: auto tracks are not stretched.
    const [a, b] = cols('auto auto', 'sem', 'bar', 'justify-content:start');
    assert(near(a, sem) && near(b, bar), `auto auto with justify-content:start keeps content widths: ${a} / ${b}`);
}
{
    // Several rows: a column's content is the widest of its items.
    const g = document.createElement('div');
    g.style.cssText = 'display:grid; width:600px; grid-template-columns:auto 1fr';
    g.innerHTML = '<span>sem</span><div>x</div><span>a much longer label here</span><div>y</div>';
    document.body.appendChild(g);
    flush();
    const w = g.children[0].getBoundingClientRect().width;
    g.remove();
    assert(near(w, long), `the auto column fits its widest item (${long}), got ${w}`);
}
console.log(`grid content tracks: sem ${sem.toFixed(1)}, long ${long.toFixed(1)}`);
