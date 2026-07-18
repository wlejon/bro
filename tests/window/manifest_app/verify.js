// Run by test_window_manifest.js in a child bro-headless whose app dir is
// manifest_app/ — asserts the bro.json window-management keys were parsed
// and applied to the (hidden) window at startup.

assert(bro.window.borderless === true, 'bro.json borderless applied');
assert(bro.window.alwaysOnTop === true, 'bro.json alwaysOnTop applied');

const min = bro.window.getMinSize();
assert(min.width === 320 && min.height === 240,
       'bro.json minWidth/minHeight applied: ' + min.width + 'x' + min.height);

const max = bro.window.getMaxSize();
assert(max.width === 1600 && max.height === 900,
       'bro.json maxWidth/maxHeight applied: ' + max.width + 'x' + max.height);

console.log('MANIFEST_WINDOW_OK');
