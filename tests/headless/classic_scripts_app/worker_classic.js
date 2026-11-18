// A classic worker: its top-level var and function declarations are properties
// of self, and so are those of the scripts it imports.
importScripts('worker_imported.js');
var wv = 'w';
function wf() {}
postMessage([self.wv, typeof self.wf, this === self, self.impVar, typeof self.impFn].join());
