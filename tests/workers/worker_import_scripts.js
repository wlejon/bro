// A classic worker that loads helpers with importScripts (Monaco's worker
// bootstrap does `importScripts(url)` from a blob: worker).
self.importOrder = 'start;';
importScripts('worker_import_lib.js');
const blob = new Blob(['self.importOrder += "blob;"; var fromBlob = 5;'], { type: 'application/javascript' });
importScripts(URL.createObjectURL(blob));
let missing = 'none';
try { importScripts('no_such_script_here.js'); } catch (e) { missing = e.name; }
let thrown = 'none';
try { importScripts('data:text/javascript,throw new RangeError("from import")'); } catch (e) { thrown = e.name + ':' + e.message; }
postMessage({ value: importedDouble(importedValue), fromBlob, order: self.importOrder, missing, thrown, type: typeof importScripts });
