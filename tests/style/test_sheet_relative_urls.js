// A relative url() in a linked stylesheet resolves against the sheet, not the
// document (CSS Values 4 §4.5.1): css/main.css's `url(img/blue.png)` is
// css/img/blue.png, an @imported css/sub/more.css's `url(../img/red.png)` is
// css/img/red.png, and a <style> element's urls stay document-relative. They
// paint, and getComputedStyle answers each as an absolute, quoted URL, as a
// browser does (Leaflet finds its marker images that way), and such a
// bro://app/ URL loads as an <img> src. The fixture app
// (sheet_url_app/) needs its own linked sheets, so it runs in a child
// bro-headless with sheet_url_app/check.js.
const cp = require('child_process');
const path = require('path');
const exe = path.join(process.env.BRO_EXE_DIR, process.platform === 'win32' ? 'bro-headless.exe' : 'bro-headless');
const app = path.join(process.env.BRO_APP_DIR, '..', 'style', 'sheet_url_app');
const r = cp.spawnSync(exe, [app, path.join(app, 'check.js')], { encoding: 'utf8' });
const out = (r.stdout || '') + (r.stderr || '');
assert(r.status === 0 && out.includes('sheet urls: ok'),
       'child exited ' + r.status + '\n' + out.split(/\r?\n/).filter((l) => /ASSERT|ERROR|ok/.test(l)).join('\n'));
console.log('test_sheet_relative_urls: OK');
