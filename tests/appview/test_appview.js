// <appview src=dir>: another app running inline in this page, as a
// compartment of its own (docs/app-isolation.md, step 3). It is who its
// bro.json says, with its own grants, and nothing mutable crosses: the page
// reaches it by message and by the functions it exposes, and the app reaches
// the page the same way.

const messages = [];
window.addEventListener('message', (e) => messages.push(e.data));

const av = document.createElement('appview');
let asked = null;
// The app's prompt is the page's to answer: this page refuses.
av.addEventListener('permissionrequest', (e) => {
  asked = e.detail;
  e.deny();
});
// A function of the page's, granted to the app as bro.app.host.add.
av.expose('add', (a, b) => a + b);
av.setAttribute('src', 'appview_child');
av.style.cssText = 'width: 200px; height: 100px';
document.body.appendChild(av);
flush();
runFrames(5);

// Who it is.
assert(av.running === true, 'the app is running');
assert(av.appId === 'org.bro.test.AppViewChild', `its id is its manifest's (${av.appId})`);
assert(av.appName === 'AppView Child', `its name is its manifest's (${av.appName})`);

// What the page reaches of it: a window to post to, and no document.
assert(av.contentDocument === null, 'the page cannot reach its document');
assert(av.contentWindow && typeof av.contentWindow.postMessage === 'function', 'its window carries postMessage');
assert(av.contentWindow.document === undefined, "its window's document is not reachable");

// What it reported.
assert(messages.length >= 1, `the app reported by message (${messages.length})`);
const r = messages[0];
assert(r.id === 'org.bro.test.AppViewChild' && r.name === 'AppView Child', `bro.app inside is its own (${r.id}, ${r.name})`);
assert(r.title === 'AppView Child', `its document is its own (${r.title})`);
assert(r.parentDocument === 'undefined', `it cannot reach the page's document (${r.parentDocument})`);
assert(r.parentPost === 'function', 'it can post to the page');
assert(r.hostAdd === 'function', `the exposed function is there (${r.hostAdd})`);
assert(r.sum === 5, `calling it ran the page's function across the membrane (${r.sum})`);

// Its grants are its own: the page holds files, the app was refused it.
assert(asked && asked.capability === 'files', `its undecided capability asked the page (${asked && asked.capability}; the app saw: ${r.files})`);
assert(asked.app === 'AppView Child', `the request names the app (${asked.app})`);
assert(typeof r.files === 'string' && r.files.startsWith('refused'), `the page's denial held (${r.files})`);
const perms = av.permissions;
assert(perms && perms.declared.includes('files'), 'its declared capabilities are its manifest\'s');
assert(perms.denied.includes('files'), `the denial is recorded (${JSON.stringify(perms)})`);
assert(require('fs').existsSync(bro.appDir + '/index.html'), "the page's own files grant is untouched");

// A message the other way, answered.
av.contentWindow.postMessage({ ping: 1 });
runFrames(5);
const echo = messages.find((m) => m && m.echo);
assert(echo && echo.echo.ping === 1, 'a message posted to its window arrives');
assert(echo.sourceIsParent === true, "the message's source is the app's own view of the page");

// Its lifecycle is the page's.
av.stop();
flush();
assert(av.running === false, 'stop() ends it');
assert(av.contentWindow === null, 'a stopped app has no window');
av.start();
flush();
runFrames(5);
assert(av.running === true, 'start() runs it again');
assert(messages.filter((m) => m && m.id).length >= 2, 'the restarted app reported again');
const again = messages.filter((m) => m && m.id).pop();
assert(again.sum === 5, 'what the page exposed is there for the restarted app too');

av.remove();
flush();
console.log('test_appview OK');
