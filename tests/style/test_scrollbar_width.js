// `scrollbar-width` sets an element scrollbar's thickness: `auto` the overlay
// default (5px, EngineConfig::elementScrollbar), `thin` half of it, `none` no bar (the element still
// scrolls), and a px length a bar that wide (a bro extension: an app for a
// 4K screen whose user cannot find, let alone drag, a 5px bar). The bar is
// hit where it is drawn, so a click on the wide track pages.

const root = document.getElementById('root');
const box = (id, sw) =>
    `<div id="${id}" style="scrollbar-width:${sw};scrollbar-color: rgb(220,0,0) rgb(0,0,220);` +
    'width:200px;height:120px;overflow-y:scroll;background:#000"><div style="height:2000px"></div></div>';
root.innerHTML = box('auto', 'auto') + box('thin', 'thin') + box('none', 'none') + box('wide', '24px');
flush();

// How wide the bar is: the run of track or thumb colour in from the right edge,
// on a row below the thumb.
function barWidth(id) {
    const r = document.getElementById(id).getBoundingClientRect();
    const y = Math.floor(r.bottom - 10);
    let n = 0;
    for (let x = Math.floor(r.right) - 1; x > r.left; x--) {
        const p = getPixel(x, y);
        if (p.b > 150 || p.r > 150) n++;
        else if (n) break;
    }
    return n;
}

const auto = barWidth('auto'), thin = barWidth('thin'), none = barWidth('none'), wide = barWidth('wide');
console.log(`scrollbar widths: auto ${auto}, thin ${thin}, none ${none}, 24px ${wide}`);
assert(auto >= 4 && auto <= 6, `auto is the 5px overlay, got ${auto}`);
assert(thin >= 2 && thin <= 3, `thin is half of it, got ${thin}`);
assert(none === 0, `none draws no bar, got ${none}`);
assert(wide >= 23 && wide <= 25, `24px is a 24px bar, got ${wide}`);

// The wide bar's thumb is at least three times its width long.
{
    const r = document.getElementById('wide').getBoundingClientRect();
    const x = Math.floor(r.right - 10);
    let thumb = 0;
    for (let y = Math.floor(r.top); y < r.bottom; y++) if (getPixel(x, y).r > 150) thumb++;
    assert(thumb >= 70, `a wide bar's thumb stays long enough to grab, got ${thumb}px`);
}

// A click on the wide track, 20px in from the edge (outside a 5px bar), pages down.
{
    const el = document.getElementById('wide');
    const r = el.getBoundingClientRect();
    click(r.right - 20, r.bottom - 10);
    flush();
    assert(el.scrollTop > 50, `a click on the wide track pages, scrollTop ${el.scrollTop}`);
}

// Bars are painted where their scroller is in the paint order: a positioned
// box over the scroller (a popup) covers its bar; the scroller's own
// positioned children (a virtual list's cells) stay under it.
{
    const host = document.createElement('div');
    host.innerHTML =
        '<div id="cells" style="position:absolute;left:300px;top:20px;width:200px;height:120px;overflow-y:scroll;' +
        'scrollbar-width:24px;scrollbar-color: rgb(220,0,0) rgb(0,0,220);background:#000">' +
        '<div style="position:relative;height:2000px">' +
        '<div style="position:absolute;left:0;top:0;width:200px;height:2000px;background:rgb(0,200,0)"></div></div></div>' +
        '<div id="pop" style="position:fixed;left:440px;top:60px;width:100px;height:40px;z-index:5;background:rgb(240,240,0)"></div>';
    root.appendChild(host);
    flush();
    const r = document.getElementById('cells').getBoundingClientRect();
    const under = getPixel(Math.floor(r.right - 10), Math.floor(r.top + 5));
    assert(under.r > 150 && under.g < 100, `the bar over the scroller's own positioned cell (thumb red): ${JSON.stringify(under)}`);
    const covered = getPixel(Math.floor(r.right - 10), 80);
    assert(covered.r > 200 && covered.g > 200 && covered.b < 60, `the popup over the bar: ${JSON.stringify(covered)}`);
    host.remove();
    flush();
}

// `none` still scrolls.
{
    const el = document.getElementById('none');
    el.scrollTop = 300;
    assert(el.scrollTop === 300, `a none bar's element still scrolls, scrollTop ${el.scrollTop}`);
}
