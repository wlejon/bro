// Test window and location globals.

// ==========================================================================
// window object
// ==========================================================================

assert(window !== undefined, 'window is defined');
assert(window === globalThis, 'window === globalThis');

// ==========================================================================
// devicePixelRatio
// ==========================================================================

assert(typeof devicePixelRatio === 'number', 'devicePixelRatio is a number');
assert(devicePixelRatio > 0, 'devicePixelRatio is positive');
// Headless pins the scale to 1.0 regardless of the desktop's real display
// scale, so test output is reproducible across machines (windowed mode
// reports the true SDL display scale instead).
assert(devicePixelRatio === 1, 'headless devicePixelRatio is a deterministic 1.0');

// ==========================================================================
// innerWidth / innerHeight
// ==========================================================================

assert(typeof innerWidth === 'number', 'innerWidth is a number');
assert(typeof innerHeight === 'number', 'innerHeight is a number');
assert(innerWidth > 0, 'innerWidth is positive');
assert(innerHeight > 0, 'innerHeight is positive');

// ==========================================================================
// navigator
// ==========================================================================

assert(typeof navigator === 'object', 'navigator is an object');
assert(typeof navigator.userAgent === 'string', 'navigator.userAgent is a string');
assert(navigator.userAgent.length > 0, 'navigator.userAgent is non-empty');
assert(typeof navigator.platform === 'string', 'navigator.platform is a string');
assert(typeof navigator.language === 'string', 'navigator.language is a string');
assert(navigator.language.length > 0, 'navigator.language is non-empty');

// ==========================================================================
// location
// ==========================================================================

assert(typeof location === 'object', 'location is an object');
assert(typeof location.href === 'string', 'location.href is a string');
assert(typeof location.origin === 'string', 'location.origin is a string');
assert(typeof location.protocol === 'string', 'location.protocol is a string');
assert(typeof location.hostname === 'string', 'location.hostname is a string');
assert(typeof location.port === 'string', 'location.port is a string');
assert(typeof location.pathname === 'string', 'location.pathname is a string');
assert(typeof location.search === 'string', 'location.search is a string');
assert(typeof location.hash === 'string', 'location.hash is a string');

// Verify expected bro:// scheme values
assert(location.protocol === 'bro:', 'location.protocol is "bro:"');
assert(location.hostname === 'app', 'location.hostname is "app"');
assert(location.pathname === '/', 'location.pathname is "/"');

// ==========================================================================
// history
// ==========================================================================

assert(typeof history === 'object', 'history is an object');
assert(typeof history.length === 'number', 'history.length is a number');
assert(history.length >= 1, 'history.length starts at 1');
