// Every synchronous function require('fs') exports is listed in
// docs/brokit-api.js. openSync / readSync / writeSync / fstatSync / closeSync
// once worked but were not listed, so an app found them by trying.

const fs = require('fs');
const path = require('path');

const docPath = path.resolve(bro.appDir, '../../docs/brokit-api.js');
assert(fs.existsSync(docPath), 'docs/brokit-api.js found at ' + docPath);
const doc = fs.readFileSync(docPath, 'utf-8');

const missing = Object.keys(fs)
    .filter((name) => /Sync$/.test(name) && typeof fs[name] === 'function')
    .filter((name) => doc.indexOf('fs.' + name + '(') < 0);
assert(missing.length === 0, 'fs functions not in docs/brokit-api.js: ' + missing.join(', '));
