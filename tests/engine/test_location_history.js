// location and history in a frame (host_location.cpp): the URL of the page
// the frame shows (bro://app/<path>, with the query of its src), history's
// pushState / replaceState changing it in place (location.search, href),
// back / forward firing popstate with the entry's state, fragment
// navigation firing hashchange, and navigation to another page of the app
// loading that page in the frame.

const el = document.createElement('iframe');
el.setAttribute('src', 'location_child?projectId=Duplex#top');
document.body.appendChild(el);
flush();
advanceTime(16);

let cw = el.contentWindow;
assert(cw, 'frame loaded');
const init = cw.__initial;
assert(init.href === 'bro://app/location_child/?projectId=Duplex#top', 'frame href, got ' + init.href);
assert(init.search === '?projectId=Duplex' && init.hash === '#top' && init.pathname === '/location_child/',
       'URL parts of the frame, got ' + JSON.stringify(init));
assert(location.href === 'bro://app/' && location.search === '', 'the app page keeps its own URL, got ' + location.href);

const loc = cw.location, hist = cw.history;
hist.replaceState({ n: 0 }, '', '?projectId=Other');
assert(loc.search === '?projectId=Other' && loc.href === 'bro://app/location_child/?projectId=Other',
       'replaceState changes the URL in place, got ' + loc.href);
assert(hist.length === 1 && hist.state.n === 0, 'replaceState keeps the length and sets the state');

hist.pushState({ n: 1 }, '', 'two?x=1');
assert(loc.pathname === '/location_child/two' && loc.search === '?x=1' && hist.length === 2,
       'pushState adds an entry, got ' + loc.href + ' length ' + hist.length);
let threw = '';
try { hist.pushState(null, '', 'https://example.com/'); } catch (e) { threw = e.name; }
assert(threw === 'SecurityError', 'pushState to another origin throws SecurityError, got ' + threw);

hist.back();
assert(loc.href === 'bro://app/location_child/?projectId=Other' && hist.state.n === 0, 'back() returns to the first entry');
hist.forward();
assert(hist.state.n === 1, 'forward() to the second');
const ev = cw.__events;
assert(ev.length === 2 && ev[0].startsWith('popstate {"n":0}') && ev[1].startsWith('popstate {"n":1}'),
       'popstate fired with each entry\'s state, got ' + JSON.stringify(ev));

loc.hash = 'here';
assert(loc.hash === '#here' && hist.length === 3, 'a fragment navigation adds an entry, got ' + loc.href);
assert(ev[2] === 'hashchange bro://app/location_child/two?x=1 -> bro://app/location_child/two?x=1#here',
       'hashchange fired with the URLs, got ' + ev[2]);

// Another page of the app: the frame loads it.
loc.href = 'sub/page.html?q=2';
flush();
advanceTime(16);
flush();
cw = el.contentWindow;
assert(cw && cw.__page, 'the frame shows the page it navigated to');
assert(cw.__page.href === 'bro://app/location_child/sub/page.html?q=2' && cw.__page.search === '?q=2',
       'the new page\'s URL, got ' + JSON.stringify(cw.__page));
assert(el.getAttribute('src') === 'location_child?projectId=Duplex#top', 'the iframe element keeps its src attribute');

el.remove();
flush();
console.log('test_location_history: OK');
