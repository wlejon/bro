// A starter headless script. Run with:
//   bro-headless <this-project-folder> run.js
//
// Headless globals: screenshot(path), advanceTime(ms), flush(), sleep(ms),
// assert(cond, msg?). Standard DOM and brokit APIs are also available.

console.log('hello from a bro-headless tool');

document.body.style.background = '#101418';
const banner = document.createElement('div');
banner.style.cssText = 'color:#a8bdff;font:48px -apple-system,Helvetica,sans-serif;padding:80px';
banner.textContent = 'rendered headlessly at ' + new Date().toISOString();
document.body.appendChild(banner);

flush();
screenshot('out.png');
console.log('wrote out.png');
