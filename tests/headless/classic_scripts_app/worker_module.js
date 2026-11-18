// A module worker: its top-level var is its own binding, not a property of
// self while its top level runs.
var mv = 1;
postMessage(typeof self.mv + ',' + mv);
