// scrollWidth / scrollHeight count the text a clip hides. CSSOM: scrollWidth
// is the width of the scrollable overflow area (content plus padding), and
// text-overflow: ellipsis is a paint effect, so the text the ellipsis stands
// for is still overflow. A nowrap ellipsis tag with max-width 200px and a long
// name reported its own width (188 + padding) where Chrome reports the full
// name's width plus padding (the cut runs no longer reached that far). Also:
// an overflow:auto nowrap line scrolls to its end, a clipped multi-line block's
// scrollHeight is its full text, and a box whose content fits reports its own
// size. The flex-item tag also checks max-width on a content-box flex item
// bounds its content box (212px border box), not its border box (it was 200).

document.body.style.margin = '0';
const near = (a, b, tol = 1.5) => Math.abs(a - b) <= tol;
const name = 'A remarkably long animal name that will not fit at all';

function mk(css, html) {
    const d = document.createElement('div');
    d.style.cssText = css;
    d.innerHTML = html;
    document.body.appendChild(d);
    flush();
    return d;
}

// The name's own width: a lone nowrap max-content box.
const probe = mk('display:inline-block; white-space:nowrap', name);
const textW = probe.getBoundingClientRect().width;
probe.remove();
assert(textW > 250, `the name is wider than the tag: ${textW}`);

// The ellipsis tag: block, inline-block, and a flex item (min-width 0).
const tagCss = 'box-sizing:content-box; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; max-width:200px; padding:0 6px';
for (const [label, el] of [
    ['block', mk(tagCss, name)],
    ['inline-block', mk('display:inline-block; ' + tagCss, name)],
    ['flex item', mk('box-sizing:content-box; display:flex; width:212px',
                     `<span style="${tagCss}; min-width:0">${name}</span>`).firstElementChild],
]) {
    assert(near(el.clientWidth, 212), `${label}: clientWidth is max-width + padding: ${el.clientWidth}`);
    assert(el.scrollWidth > el.clientWidth,
           `${label}: scrollWidth ${el.scrollWidth} > clientWidth ${el.clientWidth}`);
    assert(near(el.scrollWidth, textW + 12),
           `${label}: scrollWidth ${el.scrollWidth} ≈ text ${textW} + padding 12`);
    assert(el.scrollHeight === el.clientHeight,
           `${label}: one line, scrollHeight ${el.scrollHeight} == clientHeight ${el.clientHeight}`);
    // Can be scrolled (overflow:hidden is a scroll container) to the text's end.
    el.scrollLeft = 100000;
    flush();
    assert(near(el.scrollLeft, el.scrollWidth - el.clientWidth),
           `${label}: scrollLeft clamps to the text's end: ${el.scrollLeft}`);
    el.parentElement === document.body ? el.remove() : el.parentElement.remove();
}

// overflow:auto nowrap (no ellipsis): the line scrolls to its end.
{
    const d = mk('white-space:nowrap; overflow:auto; width:200px; padding:4px', name);
    assert(near(d.scrollWidth, textW + 8), `auto: scrollWidth ${d.scrollWidth} ≈ ${textW} + 8`);
    d.scrollLeft = 100000;
    flush();
    assert(near(d.scrollLeft, d.scrollWidth - d.clientWidth) && d.scrollLeft > 100,
           `auto: scrolled to the end of the line: ${d.scrollLeft}`);
    d.remove();
}

// overflow:auto with an ellipsis: the scroll range is the whole line too.
{
    const d = mk('white-space:nowrap; overflow:auto; text-overflow:ellipsis; width:200px', name);
    assert(near(d.scrollWidth, textW), `auto + ellipsis: scrollWidth ${d.scrollWidth} ≈ ${textW}`);
    d.remove();
}

// An inline-block child wider than a clipping box.
{
    const d = mk('white-space:nowrap; overflow:hidden; width:200px',
                 '<span style="display:inline-block; width:500px; height:10px"></span>');
    assert(near(d.scrollWidth, 500), `inline-block child: scrollWidth ${d.scrollWidth}`);
    d.remove();
}

// Clipped multi-line text: scrollHeight is every line.
{
    const d = mk('overflow:hidden; width:100px; height:40px; line-height:20px', name + ' ' + name);
    assert(d.clientHeight === 40, `multi-line: clientHeight ${d.clientHeight}`);
    assert(d.scrollHeight > 40 && d.scrollHeight % 20 === 0,
           `multi-line: scrollHeight ${d.scrollHeight} is the full lines`);
    assert(d.scrollWidth === d.clientWidth, `multi-line: wraps, scrollWidth ${d.scrollWidth}`);
    d.remove();
}

// Content that fits: scroll size is the client size.
{
    const d = mk('box-sizing:content-box; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; width:400px; padding:5px', 'short');
    assert(d.scrollWidth === d.clientWidth && d.clientWidth === 410,
           `fits: scrollWidth ${d.scrollWidth} == clientWidth ${d.clientWidth}`);
    assert(d.scrollHeight === d.clientHeight, `fits: scrollHeight ${d.scrollHeight}`);
    // And after the text grows past the box and shrinks back.
    d.textContent = name + name;
    flush();
    assert(d.scrollWidth > d.clientWidth, `grown: scrollWidth ${d.scrollWidth}`);
    d.textContent = 'short';
    flush();
    assert(d.scrollWidth === d.clientWidth, `shrunk back: scrollWidth ${d.scrollWidth}`);
    d.remove();
}
