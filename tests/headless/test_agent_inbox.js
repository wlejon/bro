// The agent inbox (docs/agent-inbox.md), driven in-process through
// controlCommand(...argv) / controlResult(id): the commands bro-ctl sends
// (`agents`, `ask`, `inbox ...`), without the socket. Held polls time out on
// the engine's clock, virtual here, so advanceTime plays them out.

const fs = require('fs');
const os = require('os');
const path = require('path');

function start() { return controlCommand.apply(null, arguments); }
function poll(id) { return controlResult(id); }
function wait(id, what) {
    for (let i = 0; i < 400; i++) {
        const r = controlResult(id);
        if (r) return r;
        advanceTime(16);
    }
    throw new Error('no reply to ' + what);
}
function run() { return wait(start.apply(null, arguments), Array.prototype.join.call(arguments, ' ')); }
function ok() {
    const r = run.apply(null, arguments);
    assert(r.ok, Array.prototype.join.call(arguments, ' ').slice(0, 80) + ' succeeds: ' + r.payload);
    return r.payload;
}
function okJson() { return JSON.parse(ok.apply(null, arguments)); }

// A files bundle (platform/inbox_bundle.h) of short ASCII files: every
// length fits one varint byte, so the bundle is a plain string.
function bundle(files) {
    let s = String.fromCharCode(files.length);
    for (const f of files) s += String.fromCharCode(f.name.length) + f.name + String.fromCharCode(f.data.length) + f.data;
    return s;
}
const scratch = path.join(os.tmpdir(), 'bro_inbox_test_' + process.pid);
fs.mkdirSync(scratch, { recursive: true });
const out = (name) => path.join(scratch, name);
const read = (p) => fs.readFileSync(p, 'latin1');

// ── nobody yet ──────────────────────────────────────────────────────────────

assert(ok('agents').indexOf('no agents') === 0, 'no agents at first');
assert(okJson('agents', '--json').length === 0, 'agents --json is an empty list');
const nobody = run('ask', 'echo', '--text=hi');
assert(!nobody.ok && nobody.payload.indexOf("no agent 'echo'") >= 0, 'asking an unknown agent fails: ' + nobody.payload);

// ── register ────────────────────────────────────────────────────────────────

const reg = okJson('inbox', 'register', 'echo', '--about=says it back');
assert(reg.name === 'echo' && reg.token.length === 32, 'register answers a token');
const token = reg.token;
const again = run('inbox', 'register', 'echo');
assert(!again.ok && again.payload.indexOf('--replace') >= 0, 'a live name is not taken twice: ' + again.payload);
assert(!run('inbox', 'register', 'bad/name').ok, 'a name with a slash is refused');
const listed = okJson('agents', '--json');
assert(listed.length === 1 && listed[0].name === 'echo' && listed[0].about === 'says it back',
       'agents lists it with its description: ' + JSON.stringify(listed));
assert(listed[0].by.via === 'app', 'registered in-process: ' + JSON.stringify(listed[0].by));

// ── asynchronous: an id now, status and result later ───────────────────────

const files = bundle([{ name: 'note.txt', data: 'hello file' }]);
const asked = okJson('ask', 'echo', '--text=--looks like an option', '--from=tester', '--origin=me@here',
                     '--files=' + files);
assert(asked.state === 'queued' && asked.id, 'ask answers the id at once: ' + JSON.stringify(asked));
assert(asked.request === '--looks like an option', 'a text starting with -- stays whole: ' + asked.request);
assert(asked.from.agent === 'tester' && asked.from.origin === 'me@here' && asked.from.via === 'app',
       'the request carries its sender: ' + JSON.stringify(asked.from));
assert(asked.from.pid > 0 && asked.from.user, 'and the transport facts: ' + JSON.stringify(asked.from));
const id = asked.id;

const got = okJson('inbox', 'next', token, out('next1.bin'), '--timeout=5000');
assert(got.type === 'request' && got.id === id && got.text === '--looks like an option',
       'next hands the agent the request: ' + JSON.stringify(got));
assert(got.from.agent === 'tester', 'with who sent it');
assert(got.files.length === 1 && got.files[0].name === 'note.txt' && got.files[0].size === 10,
       'and its files: ' + JSON.stringify(got.files));
assert(read(out('next1.bin')) === files, 'the files arrive as the bundle that was sent');
assert(okJson('inbox', 'status', id).state === 'working', 'delivered: working');

// A status poll held until the next progress line.
const held = start('inbox', 'status', id, '--since=0', '--wait=20');
advanceTime(100);
assert(poll(held) === null, 'status --wait holds while nothing changes');
okJson('inbox', 'progress', token, id, '--text=half way');
const st = wait(held, 'held status');
assert(st.ok, 'the held status answers');
const sj = JSON.parse(st.payload);
assert(sj.progress.length === 1 && sj.progress[0].text === 'half way' && sj.progressNext === 1,
       'with the progress line: ' + st.payload);

const replyFiles = bundle([{ name: 'out.txt', data: 'result bytes' }, { name: 'b.txt', data: 'x' }]);
okJson('inbox', 'reply', token, id, '--text=done: ok', '--files=' + replyFiles);
const res = okJson('inbox', 'result', id, out('result1.bin'));
assert(res.state === 'done' && res.text === 'done: ok', 'result: ' + JSON.stringify(res));
assert(res.files.length === 2 && res.files[0].name === 'out.txt', 'the reply names its files');
assert(read(out('result1.bin')) === replyFiles, 'the reply files come back as sent');
assert(!run('inbox', 'reply', token, id, '--text=twice').ok, 'a request is answered once');

// ── synchronous: ask --wait holds until the reply ──────────────────────────

const sync = start('ask', 'echo', out('sync.bin'), '--text=ping', '--wait=30');
advanceTime(50);
assert(poll(sync) === null, 'ask --wait holds until the agent answers');
const g2 = okJson('inbox', 'next', token, out('next2.bin'));
assert(g2.text === 'ping', 'the agent gets it');
okJson('inbox', 'reply', token, g2.id, '--text=pong');
const sr = wait(sync, 'ask --wait');
assert(sr.ok && JSON.parse(sr.payload).text === 'pong', 'ask --wait answers the reply: ' + sr.payload);
assert(read(out('sync.bin')).charCodeAt(0) === 0, 'no files: an empty bundle');

// A failed answer.
const f = okJson('ask', 'echo', '--text=boom');
okJson('inbox', 'next', token, out('n3.bin'));
okJson('inbox', 'reply', token, f.id, '--text=it broke', '--failed');
const fr = okJson('inbox', 'result', f.id);
assert(fr.state === 'failed' && fr.error === 'it broke', 'a failed reply: ' + JSON.stringify(fr));

// ── the next poll is held, and answered when a request arrives ─────────────

const waiting = start('inbox', 'next', token, out('n4.bin'), '--timeout=20000');
advanceTime(100);
assert(poll(waiting) === null, 'next holds with nothing to do');
const late = okJson('ask', 'echo', '--text=late');
const wr = wait(waiting, 'held next');
assert(wr.ok && JSON.parse(wr.payload).id === late.id, 'a held next takes the request as it arrives');

// ── cancel: the agent hears about it; its late reply is refused ─────────────

okJson('inbox', 'cancel', late.id, '--from=tester');
assert(okJson('inbox', 'status', late.id).state === 'cancelled', 'cancelled');
const cn = okJson('inbox', 'next', token, out('n5.bin'));
assert(cn.type === 'cancel' && cn.id === late.id, 'next tells the agent: ' + JSON.stringify(cn));
const lateReply = run('inbox', 'reply', token, late.id, '--text=too late');
assert(!lateReply.ok && lateReply.payload.indexOf('cancelled') >= 0, 'a reply to a cancelled request is refused');

// An idle poll times out empty.
const idle = start('inbox', 'next', token, out('n6.bin'), '--timeout=1000');
advanceTime(1200);
const ir = wait(idle, 'idle next');
assert(ir.ok && JSON.parse(ir.payload).type === 'idle', 'an empty poll times out idle: ' + ir.payload);

// ── the event feed (what helm shows) ────────────────────────────────────────

const ev = okJson('inbox', 'events', '--since=0');
const kinds = ev.events.map((e) => e.kind);
for (const k of ['registered', 'asked', 'delivered', 'progress', 'done', 'failed', 'cancelled'])
    assert(kinds.indexOf(k) >= 0, 'the feed has ' + k + ': ' + kinds.join(','));
const askedEv = ev.events.find((e) => e.kind === 'asked' && e.id === id);
assert(askedEv && askedEv.to === 'echo' && askedEv.from === 'tester', 'an event names the agents: ' + JSON.stringify(askedEv));
const heldEv = start('inbox', 'events', '--since=' + ev.next, '--wait=20');
advanceTime(50);
assert(poll(heldEv) === null, 'events --wait holds with nothing new');
okJson('ask', 'echo', '--text=one more');
const he = wait(heldEv, 'held events');
assert(he.ok && JSON.parse(he.payload).events[0].kind === 'asked', 'and answers the next event: ' + he.payload);

// ── the audit log ───────────────────────────────────────────────────────────

const lines = ok('inbox', 'log', '--id=' + id).trim().split('\n').map((l) => JSON.parse(l));
const evs = lines.map((l) => l.event);
assert(evs.join(',') === 'ask,deliver,progress,reply', 'the log has the request\'s life: ' + evs.join(','));
assert(lines[0].from.agent === 'tester' && lines[0].from.via === 'app' && lines[0].files[0].name === 'note.txt',
       'the log line says who asked and what came with it: ' + JSON.stringify(lines[0]));
assert(lines[3].text === 'done: ok' && lines[0].t > 0, 'and the reply');
const all = ok('inbox', 'log', '--limit=2').trim().split('\n');
assert(all.length === 2, 'log --limit keeps the last lines');

// ── replace, unregister, and the lease ──────────────────────────────────────

const queued = okJson('ask', 'echo', '--text=waits');
const reg2 = okJson('inbox', 'register', 'echo', '--replace', '--about=second');
assert(reg2.token !== token, 'replace takes the name with a new token');
assert(!run('inbox', 'next', token).ok, 'the old token is dead');
const moved = okJson('inbox', 'next', reg2.token, out('n7.bin'));
assert(moved.type === 'request' && (moved.id === queued.id || moved.text === 'one more'),
       'what was queued goes to the new agent: ' + JSON.stringify(moved));
okJson('inbox', 'unregister', reg2.token);
assert(okJson('agents', '--json').length === 0, 'unregistered');
const left = okJson('inbox', 'result', moved.id);
assert(left.state === 'failed' && left.error.indexOf('left') >= 0, 'what it held fails when it leaves: ' + JSON.stringify(left));

const reg3 = okJson('inbox', 'register', 'sleepy');
const lost = okJson('ask', 'sleepy', '--text=anyone?');
advanceTime(46000);
assert(okJson('agents', '--json').length === 0, 'a registration lapses when its agent stops polling');
assert(okJson('inbox', 'result', lost.id).state === 'failed', 'and its queued requests fail');
assert(!run('inbox', 'next', reg3.token).ok, 'its token is dead');

fs.rmSync(scratch, { recursive: true, force: true });
