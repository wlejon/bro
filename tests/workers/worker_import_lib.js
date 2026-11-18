// Imported by worker_import_scripts.js through importScripts: its top-level
// declarations land in the worker's global scope.
var importedValue = 21;
function importedDouble(x) { return x * 2; }
self.importOrder = (self.importOrder || '') + 'lib;';
