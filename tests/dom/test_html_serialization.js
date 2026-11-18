// innerHTML / outerHTML and XMLSerializer write markup as Chromium does: text
// escaped outside the raw-text elements, &nbsp;, void elements without end
// tags, attributes escaped; XML with namespace declarations and empty
// elements closed.
const d = document.createElement('div');
d.textContent = 'a<b & c> "q\'';
const s = document.createElement('script');
s.textContent = '1<2&';
d.appendChild(s);
const st = document.createElement('style');
st.textContent = 'a>b{}';
d.appendChild(st);
d.appendChild(document.createElement('br'));
d.appendChild(document.createElement('img'));
d.appendChild(document.createElement('input'));
d.setAttribute('title', 'x "y<z>&\'');
d.appendChild(document.createComment('c<'));
const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
const lg = document.createElementNS('http://www.w3.org/2000/svg', 'linearGradient');
lg.setAttribute('gradientUnits', 'userSpaceOnUse');
svg.appendChild(lg);
svg.appendChild(document.createElementNS('http://www.w3.org/2000/svg', 'rect'));
d.appendChild(svg);
const t = document.createElement('template');
t.innerHTML = '<p>x</p>';
d.appendChild(t);
const ta = document.createElement('textarea');
ta.textContent = '<i>';
d.appendChild(ta);
const html = '<div title="x&nbsp;&quot;y&lt;z&gt;&amp;\'">a&lt;b &amp; c&gt;&nbsp;"q\'<script>1<2&</script><style>a>b{}</style>' +
    '<br><img><input><!--c<--><svg><linearGradient gradientUnits="userSpaceOnUse"></linearGradient><rect></rect></svg>' +
    '<template><p>x</p></template><textarea>&lt;i&gt;</textarea></div>';
assert(d.outerHTML === html, 'outerHTML: ' + d.outerHTML);
const xml = '<div xmlns="http://www.w3.org/1999/xhtml" title="x &quot;y&lt;z&gt;&amp;\'">a&lt;b &amp; c&gt; "q\'' +
    '<script>1&lt;2&amp;</script><style>a&gt;b{}</style><br /><img /><input /><!--c<-->' +
    '<svg xmlns="http://www.w3.org/2000/svg"><linearGradient gradientUnits="userSpaceOnUse"/><rect/></svg>' +
    '<template><p>x</p></template><textarea>&lt;i&gt;</textarea></div>';
const gotXml = new XMLSerializer().serializeToString(d);
assert(gotXml === xml, 'XMLSerializer: ' + gotXml);
assert(new XMLSerializer().serializeToString(document.createTextNode('a<&>')) === 'a&lt;&amp;&gt;', 'a text node');
// The HTML-escaping idiom: textContent in, innerHTML out.
const esc = document.createElement('div');
esc.textContent = '<img src=x onerror=alert(1)>';
assert(esc.innerHTML === '&lt;img src=x onerror=alert(1)&gt;', 'textContent is escaped by innerHTML');
console.log('test_html_serialization: OK');
