// URL attributes read resolved (a/area/link href, img src) and an anchor's
// URL parts, as in a browser: libraries parse URLs through an <a>.
const a = (h) => { const e = document.createElement('a'); if (h !== null) e.setAttribute('href', h); return e; };
const here = location.href.replace(/[^/]*$/, '');
const e = a('dir/x.html?q=1#h');
assert(e.href === here + 'dir/x.html?q=1#h', 'href resolved: ' + e.href);
assert(e.pathname.endsWith('/dir/x.html') && e.search === '?q=1' && e.hash === '#h', 'parts: ' + [e.pathname, e.search, e.hash]);
const m = a('mailto:a@b');
assert(m.href === 'mailto:a@b' && m.protocol === 'mailto:' && m.pathname === 'a@b' && m.host === '', 'mailto: ' + [m.href, m.pathname]);
assert(a('javascript:void(0)').href === 'javascript:void(0)', 'javascript:');
assert(a('http://example.com/p').host === 'example.com', 'absolute');
assert(a(null).href === '', 'no href');
const w = a('x');
w.pathname = '/new';
assert(w.href.endsWith('/new'), 'pathname setter: ' + w.href);
w.hash = 'k';
assert(w.href.endsWith('/new#k'), 'hash setter: ' + w.href);
const img = document.createElement('img');
img.src = 'i.png';
assert(img.src === here + 'i.png', 'img.src: ' + img.src);
const l = document.createElement('link');
l.href = 's.css';
assert(l.href === here + 's.css' && l.getAttribute('href') === 's.css', 'link.href');
assert(document.createElement('base').href === location.href, 'base without href');
assert(String(a('y')) === here + 'y', 'an anchor stringifies to its href');
assert(new URL('mailto:a@b').origin === 'null' && new URL('data:text/plain,hi').pathname === 'text/plain,hi', 'opaque URLs');
console.log('test_url_reflection: OK');
