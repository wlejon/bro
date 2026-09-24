// Each shadow root's <style> applies inside that root only, whichever way the
// markup got there (shadowRoot.innerHTML or a template clone); and a slotted
// light-DOM child inherits through the flat tree, from the element wrapping
// its <slot>, not from the host.

const root = document.getElementById('root');

function makeHost(css, useTemplate) {
    const host = document.createElement('div');
    root.appendChild(host);
    const sr = host.attachShadow({ mode: 'open' });
    const markup = `<style>${css}</style><b class="x">t</b>`;
    if (useTemplate) {
        const t = document.createElement('template');
        t.innerHTML = markup;
        sr.appendChild(t.content.cloneNode(true));
    } else {
        sr.innerHTML = markup;
    }
    return sr;
}

const red = makeHost('.x { color: rgb(255, 0, 0); }', false);
const blue = makeHost('.x { color: rgb(0, 0, 255); }', false);
const green = makeHost('.x { color: rgb(0, 128, 0); }', true);
const outside = document.createElement('b');
outside.className = 'x';
root.appendChild(outside);
flush();

const col = (el) => getComputedStyle(el).color;
assert(col(red.querySelector('.x')) === 'rgb(255, 0, 0)', 'first shadow root keeps its red, got ' + col(red.querySelector('.x')));
assert(col(blue.querySelector('.x')) === 'rgb(0, 0, 255)', 'second shadow root is blue, got ' + col(blue.querySelector('.x')));
assert(col(green.querySelector('.x')) === 'rgb(0, 128, 0)', 'template-cloned shadow root is green, got ' + col(green.querySelector('.x')));
assert(!['rgb(255, 0, 0)', 'rgb(0, 0, 255)', 'rgb(0, 128, 0)'].includes(col(outside)),
    'no shadow sheet reaches the document, got ' + col(outside));

// --- slotted inheritance ------------------------------------------------------
const card = document.createElement('div');
card.style.color = 'rgb(10, 10, 10)';
card.style.fontWeight = '400';
card.innerHTML = '<span slot="title" id="title">Title</span><span id="loose">x</span>';
root.appendChild(card);
const csr = card.attachShadow({ mode: 'open' });
csr.innerHTML = '<h3 style="color: rgb(200, 0, 0); font-weight: 700"><slot name="title"></slot></h3><slot></slot>';
flush();

const title = document.getElementById('title');
const cs = getComputedStyle(title);
assert(cs.color === 'rgb(200, 0, 0)', 'slotted child inherits the slot parent colour, got ' + cs.color);
assert(cs.fontWeight === '700', 'and its weight, got ' + cs.fontWeight);
const loose = getComputedStyle(document.getElementById('loose'));
assert(loose.color === 'rgb(10, 10, 10)', 'a child in a bare default slot still inherits the host, got ' + loose.color);

// A change on the slot's wrapper reaches the slotted child.
csr.querySelector('h3').style.color = 'rgb(0, 90, 0)';
flush();
assert(getComputedStyle(title).color === 'rgb(0, 90, 0)', 'restyling the wrapper re-inherits, got ' + getComputedStyle(title).color);
