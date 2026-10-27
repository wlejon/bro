// A stopped app gives back everything it held (docs/app-isolation.md, step
// 5): its canvases, its sound, its Worker, its timers, its
// watchers and its child processes, whether its page stopped it with stop()
// or the runtime terminated it for not returning. Checked from outside: the
// child process is gone and the Worker's heartbeat file stops changing.

const fs = require('fs');
const messages = [];
window.addEventListener('message', (e) => messages.push(e.data));

const av = document.createElement('appview');
av.addEventListener('permissionrequest', (e) => e.allow());
let terminated = null;
av.addEventListener('terminated', (e) => {
  terminated = e.detail;
});
av.setAttribute('src', 'appview_ledger');
av.style.cssText = 'position: absolute; left: 0; top: 0; width: 300px; height: 120px';
document.body.appendChild(av);

const alive = (pid) => {
  try {
    process.kill(pid, 0);
    return true;
  } catch (e) {
    return e.code === 'EPERM';
  }
};

function open(round) {
  messages.length = 0;
  flush();
  let frames = 0;
  while (!messages.some((m) => m && m.opened) && frames < 500) {
    runFrames(1);
    wallSleep(5);
    frames++;
  }
  const r = messages.find((m) => m && m.opened);
  assert(r, `${round}: the app reported what it opened`);
  for (const what of ['canvas2d', 'audio', 'interval', 'watch', 'worker', 'terminal'])
    assert(r.opened.includes(what), `${round}: it opened ${what} (${r.opened}; terminal: ${r.terminal})`);
  // Not yet in a sub-document (docs/appview-api.js); this changes when they are.
  assert(!r.opened.includes('webgl2') && !r.opened.includes('scene'), `${round}: WebGL and the scene answer null`);
  assert(r.pid > 0 && alive(r.pid), `${round}: its child process runs (${r.pid})`);
  // The heartbeat is beating before the app stops.
  wallSleep(100);
  const before = fs.readFileSync(r.heartbeat, 'utf8');
  wallSleep(100);
  assert(fs.readFileSync(r.heartbeat, 'utf8') !== before, `${round}: the worker's heartbeat beats`);
  return r;
}

function released(round, r) {
  runFrames(3);
  let waited = 0;
  while (alive(r.pid) && waited < 2000) {
    wallSleep(20);
    waited += 20;
  }
  assert(!alive(r.pid), `${round}: its child process is gone (${r.pid}, after ${waited} ms)`);
  wallSleep(100);
  const stopped = fs.readFileSync(r.heartbeat, 'utf8');
  wallSleep(200);
  runFrames(2);
  assert(fs.readFileSync(r.heartbeat, 'utf8') === stopped, `${round}: its worker stopped beating`);
  assert(av.running === false && av.contentWindow === null, `${round}: it is not running`);
}

// Stopped by its page.
let r = open('stop()');
av.stop();
released('stop()', r);

// Terminated by the runtime, everything still open.
av.start();
r = open('terminated');
const box = av.getBoundingClientRect();
click(box.x + 250, box.y + 100);
runFrames(2);
assert(terminated && /did not return/.test(terminated.reason), `the page heard it was stopped (${terminated && terminated.reason})`);
released('terminated', r);

// And it runs again, opening all of it afresh.
av.start();
r = open('restarted');
av.remove();
released('removed', r);

console.log('test_appview_teardown OK');
