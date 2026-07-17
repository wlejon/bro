// @media (prefers-color-scheme) — CSS side (htmlayout MediaContext.colorScheme)
// plus the bro plumbing: the appearance.colorScheme setting (system|light|dark)
// overrides the OS theme, and changing it at runtime re-evaluates every
// retained sheet and restyles.

const root = document.getElementById('root');

// The setting exists, defaults to "system", and surfaces via getAll.
const initial = bro.settings.get('appearance.colorScheme');
assert(initial === 'system' || initial === 'light' || initial === 'dark',
       'appearance.colorScheme resolves to a known value, got ' + initial);
const cat = bro.settings.getAll('appearance');
assert(typeof cat === 'object' && typeof cat.colorScheme === 'string',
       'getAll(appearance) has colorScheme');
assert(typeof bro.settings.getAll().appearance === 'object',
       'getAll() includes appearance category');

// Invalid values are rejected (not applied).
bro.settings.set('appearance.colorScheme', 'blorp');
assert(bro.settings.get('appearance.colorScheme') !== 'blorp',
       'invalid colorScheme value rejected');

// Inject a sheet with a dark-scheme override and a probe element.
const style = document.createElement('style');
style.textContent = `
  #pcs-box { width: 60px; height: 40px; background-color: rgb(10, 20, 30); }
  @media (prefers-color-scheme: dark) {
    #pcs-box { background-color: rgb(200, 100, 50); }
  }
`;
document.head.appendChild(style);
const box = document.createElement('div');
box.id = 'pcs-box';
root.appendChild(box);

// Force light: the @media block must not apply.
bro.settings.set('appearance.colorScheme', 'light');
flush();
let bg = getComputedStyle(box).backgroundColor;
assert(bg.indexOf('10') !== -1 && bg.indexOf('200') === -1,
       'light scheme: base background applies, got ' + bg);

// Pixel-verify the light background actually painted.
let r = box.getBoundingClientRect();
let px = getPixel(Math.floor(r.left + r.width / 2), Math.floor(r.top + r.height / 2));
assert(px.r < 60 && px.g < 60 && px.b < 80,
       `light scheme pixel is dark navy, got rgb(${px.r},${px.g},${px.b})`);

// Flip to dark at runtime: the change callback must re-evaluate @media and
// restyle without any explicit reload.
bro.settings.set('appearance.colorScheme', 'dark');
flush();
bg = getComputedStyle(box).backgroundColor;
assert(bg.indexOf('200') !== -1,
       'dark scheme: @media (prefers-color-scheme: dark) applies, got ' + bg);

px = getPixel(Math.floor(r.left + r.width / 2), Math.floor(r.top + r.height / 2));
assert(px.r > 150 && px.b < 120,
       `dark scheme pixel is orange, got rgb(${px.r},${px.g},${px.b})`);

// Back to light: flips again (not latched).
bro.settings.set('appearance.colorScheme', 'light');
flush();
bg = getComputedStyle(box).backgroundColor;
assert(bg.indexOf('200') === -1, 'flipping back to light restores base, got ' + bg);

// "system" resolves to whatever the OS reports — must be one of the two
// schemes and must not throw.
bro.settings.set('appearance.colorScheme', 'system');
flush();
bg = getComputedStyle(box).backgroundColor;
assert(bg.indexOf('10') !== -1 || bg.indexOf('200') !== -1,
       'system scheme resolves to light or dark, got ' + bg);

// A dynamically-injected sheet (added AFTER the scheme was set) also gets
// media-filtered against the current scheme.
bro.settings.set('appearance.colorScheme', 'dark');
const style2 = document.createElement('style');
style2.textContent =
  '@media (prefers-color-scheme: dark) { #pcs-box { width: 90px; } }';
document.head.appendChild(style2);
flush();
r = box.getBoundingClientRect();
assert(Math.round(r.width) === 90,
       'late-injected sheet media-filtered against current scheme, got ' + r.width);

// Cleanup: drop the user override (reverts to system default).
bro.settings.reset('appearance');
assert(bro.settings.get('appearance.colorScheme') === 'system',
       'reset(appearance) reverts to system');
root.innerHTML = '';

console.log('PASS prefers-color-scheme');
