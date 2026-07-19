// Menu bar in headless: bro.menu.show() reserves the same contentTop()
// inset and renders the same menu panel as windowed mode. Headless used
// to force the bar off regardless of bro.menu state, which made headless
// geometry diverge from windowed and hid inset-dependent compositing bugs
// (e.g. canvas layers drawn 28px above their element) from every headless
// test. Exercises Engine::contentInsets() + isSystemDocVisible().

const MENU_H = 28;   // MenuBar::height (engine/menu_bar.h)

// Solid, unmistakable app content: red body edge to edge.
document.body.style.margin = '0';
document.body.style.background = '#ff0000';
flush();

const isRed = (p) => p.r > 200 && p.g < 50 && p.b < 50;

// --- default: hidden — app content owns the full frame ---------------------
assert(bro.menu.visible === false, 'menu hidden by default');
const H0 = innerHeight;
let p = getPixel(50, 5);
assert(isRed(p), 'menu hidden: app content at the very top of the frame, got ' +
       JSON.stringify(p));

// --- show(): inset reserved, resize dispatched, panel rendered -------------
let resizes = 0;
window.addEventListener('resize', () => resizes++);

bro.menu.show();
flush();

assert(bro.menu.visible === true, 'visible after show()');
assert(innerHeight === H0 - MENU_H,
       'innerHeight shrinks by the menu height (' + innerHeight + ' vs ' +
       (H0 - MENU_H) + ')');
assert(resizes >= 1, 'resize event fired on menu show');

// Frame space: the chrome strip itself. getPixel() is document-space (it
// shares getBoundingClientRect()'s origin), so it cannot see the menu bar at
// all once the inset exists — probing the chrome is what getFramePixel is for.
p = getFramePixel(50, 5);
assert(!isRed(p), 'menu bar occupies the top inset (not app content), got ' +
       JSON.stringify(p));
p = getFramePixel(50, MENU_H + 4);
assert(isRed(p), 'app content starts directly below the inset, got ' +
       JSON.stringify(p));

// Document space: the same app pixel, addressed the way an app test would.
// y=4 is inside the document because the document now starts below the menu.
p = getPixel(50, 4);
assert(isRed(p), 'document y=4 is app content regardless of the inset, got ' +
       JSON.stringify(p));

// --- hide(): full frame returns to the app ---------------------------------
bro.menu.hide();
flush();

assert(bro.menu.visible === false, 'hidden after hide()');
assert(innerHeight === H0, 'innerHeight restored on hide');
p = getPixel(50, 5);
assert(isRed(p), 'app content back at the top after hide, got ' +
       JSON.stringify(p));
