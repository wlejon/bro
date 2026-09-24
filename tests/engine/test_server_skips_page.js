// bro-server runs the server script, not the app's page: index.html's scripts
// are written for a renderer, and a throw there used to fail the server
// script ("failed to evaluate script") after it had run. With no script named,
// bro-server runs the app's server.js.

const cp = require('child_process');
const path = require('path');

const exeName = process.platform === 'win32' ? 'bro-server.exe' : 'bro-server';
const exe = path.join(process.env.BRO_EXE_DIR, exeName);
const appDir = path.join(process.env.BRO_APP_DIR, '..', 'engine', 'server_app');
const serverJs = path.join(appDir, 'server.js');

function run(args, label) {
    const r = cp.spawnSync(exe, args, { encoding: 'utf8', timeout: 60000 });
    const out = (r.stdout || '') + (r.stderr || '');
    assert(r.status === 0, label + ': bro-server exited ' + r.status + '\n--- output ---\n' + out);
    assert(out.includes('SERVER_APP_SERVER_RAN object'), label + ': the server script ran\n' + out);
    assert(!out.includes('SERVER_APP_PAGE_RAN'), label + ': the page script did not run\n' + out);
    assert(!out.includes('failed to evaluate'), label + ': no evaluation failure\n' + out);
}

run([appDir, serverJs], 'explicit server.js');
run([appDir], 'default server.js');

console.log('test_server_skips_page: OK');
