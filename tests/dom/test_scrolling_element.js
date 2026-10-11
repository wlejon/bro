// document.scrollingElement (CSSOM View): the root element in a
// standards-mode document, and its scrollTop is the viewport's. It used to be
// undefined, so code scrolling the page through it threw.

assert(document.compatMode === 'CSS1Compat', 'documents are in standards mode');
assert(document.scrollingElement === document.documentElement,
       'document.scrollingElement is the root element');

// A page taller than the viewport, scrolled through it.
const tall = document.createElement('div');
tall.style.height = '5000px';
document.body.appendChild(tall);
flush();

document.scrollingElement.scrollTop = 300;
flush();
assert(window.scrollY === 300, `scrollingElement.scrollTop scrolls the viewport, scrollY ${window.scrollY}`);
assert(document.scrollingElement.scrollTop === 300, 'and reads it back');

window.scrollTo(0, 120);
flush();
assert(document.scrollingElement.scrollTop === 120,
       `the viewport's scroll reads through it, got ${document.scrollingElement.scrollTop}`);

document.scrollingElement.scrollTop = 0;
flush();
assert(window.scrollY === 0, 'back to the top');
tall.remove();
console.log('scrollingElement: checked');
