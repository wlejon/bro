// window.open — shell URL handoff via SDL_OpenURL.
// Headless never shells out: returns null without launching anything.

assert(typeof window.open === 'function', 'window.open exists');
assert(window.open('https://example.com') === null, 'open(url) returns null');
assert(window.open('mailto:someone@example.com') === null, 'open(mailto) returns null');
assert(window.open() === null, 'open() returns null');
assert(window.open(null) === null, 'open(null) returns null');
assert(window.open('', '_blank', 'noopener') === null, 'extra args ignored');

console.log('window.open OK');
