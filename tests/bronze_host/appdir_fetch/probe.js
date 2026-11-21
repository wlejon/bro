// The fetch probe: a bronze-compiled app that tests fetch() over engine asset mounts.
//
// It is the subject of run_fetch_test.sh, proving that compiled apps can fetch assets,
// receive real bronze Promise objects resolving to Response, and consume them via
// text(), json(), and arrayBuffer().

function say(label, value) { console.log('APP ' + label + '=' + value); }

// --- 1. Fetch text asset ----------------------------------------------------
fetch('test_text.txt').then(function (res) {
    say('text.ok', res.ok);
    say('text.status', res.status);
    return res.text();
}).then(function (text) {
    say('text.body', text);
});

// --- 2. Fetch JSON asset ----------------------------------------------------
fetch('test_json.json').then(function (res) {
    say('json.ok', res.ok);
    say('json.status', res.status);
    return res.json();
}).then(function (data) {
    say('json.name', data.name);
    say('json.version', data.version);
    say('json.active', data.active);
});

// --- 3. Fetch binary asset as ArrayBuffer -----------------------------------
fetch('test_bin.bin').then(function (res) {
    say('bin.ok', res.ok);
    say('bin.status', res.status);
    return res.arrayBuffer();
}).then(function (buf) {
    say('bin.byteLength', buf.byteLength);
});

// --- 4. Fetch missing file (404) --------------------------------------------
fetch('missing_file.txt').then(function (res) {
    say('404.ok', res.ok);
    say('404.status', res.status);
});

// --- 5. Fetch invalid JSON and catch SyntaxError ----------------------------
fetch('test_bad.json').then(function (res) {
    return res.json();
}).then(function () {
    say('bad_json.unreachable', true);
}).catch(function () {
    say('bad_json.caught', true);
});

// --- Top level done ---------------------------------------------------------
say('ready', 1);
