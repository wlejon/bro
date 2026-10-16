// bro.inbox (docs/inbox-api.js): the agent inbox from a page. Headless uses
// its own inbox in-process; replies reach their promises through a frame
// pump, so each step advances time until its promise settles.

function settle(promise, what) {
    let done = false, value, error;
    promise.then((v) => { done = true; value = v; }, (e) => { done = true; error = e; });
    for (let i = 0; i < 3000 && !done; i++) advanceTime(16);
    if (!done) throw new Error('never settled: ' + what);
    if (error) throw error;
    return value;
}

assert(typeof bro.inbox === 'object' && typeof bro.inbox.serve === 'function', 'bro.inbox is there');
assert(bro.inbox === bro.inbox, 'compiled once');

const seen = [];
const watcher = bro.inbox.watch((e) => seen.push(e));
for (let i = 0; i < 10; i++) advanceTime(16);

let progressed = false;
const agent = settle(bro.inbox.serve('upper', { about: 'shouts it back' }, async (req, ctx) => {
    await ctx.progress('working on ' + req.id);
    progressed = true;
    if (req.text === 'fail please') throw new Error('as asked');
    const files = req.files.map((f) => ({ name: 'up-' + f.name, data: new TextDecoder().decode(f.data).toUpperCase() }));
    return { text: req.text.toUpperCase(), files };
}), 'serve');
assert(agent.name === 'upper', 'serve answers a handle');

const agents = settle(bro.inbox.agents(), 'agents');
assert(agents.length === 1 && agents[0].about === 'shouts it back', 'agents: ' + JSON.stringify(agents));

const bytes = new TextEncoder().encode('small file');
const r = settle(bro.inbox.ask('upper', 'hello', { wait: 30, files: [{ name: 'a.txt', data: bytes }] }), 'ask');
assert(r.state === 'done' && r.text === 'HELLO', 'ask with wait answers: ' + JSON.stringify(r));
assert(progressed, 'the handler reported progress');
assert(r.files.length === 1 && r.files[0].name === 'up-a.txt', 'the reply carries files: ' + JSON.stringify(r.files));
assert(new TextDecoder().decode(r.files[0].data) === 'SMALL FILE', 'with their bytes');
assert(r.from.via === 'app' && r.from.agent.length > 0, 'sent in-process as the app: ' + JSON.stringify(r.from));

const queued = settle(bro.inbox.ask('upper', 'fail please'), 'async ask');
assert(queued.id, 'without wait: the id now');
const failed = settle(bro.inbox.result(queued.id, { wait: 30 }), 'result');
assert(failed.state === 'failed' && failed.error === 'as asked', 'a throwing handler fails the request: ' + JSON.stringify(failed));
const st = settle(bro.inbox.status(queued.id), 'status');
assert(st.progress.length === 1, 'status has its progress');

const log = settle(bro.inbox.log({ id: queued.id }), 'log');
assert(log.map((l) => l.event).join(',') === 'ask,deliver,progress,fail', 'the audit log: ' + log.map((l) => l.event).join(','));

for (let i = 0; i < 200 && !seen.some((e) => e.kind === 'failed'); i++) advanceTime(16);
assert(seen.some((e) => e.kind === 'asked') && seen.some((e) => e.kind === 'done') && seen.some((e) => e.kind === 'failed'),
       'watch saw the traffic: ' + seen.map((e) => e.kind).join(','));
watcher.stop();

settle(agent.close(), 'close');
assert(settle(bro.inbox.agents(), 'agents after').length === 0, 'close unregisters');
let threw = '';
try { settle(bro.inbox.ask('upper', 'anyone?'), 'ask gone'); } catch (e) { threw = e.message; }
assert(/no agent/.test(threw), 'asking a gone agent rejects: ' + threw);
