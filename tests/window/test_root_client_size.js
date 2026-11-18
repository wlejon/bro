// The root element's clientWidth / clientHeight are the viewport's (CSSOM
// View, a standards-mode document), however short the page is: jQuery's
// $(window).height() reads documentElement.clientHeight, and Hextris sized its
// canvas to a 16 px tall page.

document.body.innerHTML = '<div id="short" style="height:16px">short page</div>';
document.body.style.margin = '0';
flush();
const html = document.documentElement;
assert(html.clientHeight === window.innerHeight,
       'documentElement.clientHeight is the viewport height ' + window.innerHeight + ', got ' + html.clientHeight);
assert(html.clientWidth > 0 && html.clientWidth <= window.innerWidth,
       'documentElement.clientWidth is the viewport width less any scrollbar, got ' + html.clientWidth);
const short = document.getElementById('short');
assert(short.clientHeight === 16, 'another element answers its own box, got ' + short.clientHeight);

console.log('test_root_client_size: OK');
