// Run by tests/brokit/test_require_script_dir.js as `bro-headless <app>
// require_dir/script.js` from a cwd elsewhere: its require of a sibling
// resolves against this file's directory, not the cwd.
const helper = require('./helper');
const json = require('./data.json');
console.log('REQUIRE_DIR ' + helper.where + ' ' + json.name);
