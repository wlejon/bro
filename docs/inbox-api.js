/**
 * bro.inbox: the session's agent inbox from a page (docs/agent-inbox.md).
 *
 * Agents in a session register by name and take requests; other agents
 * list them and ask them things, with text and files going both ways. In the
 * display's own realm (helm's UI) every call goes to that engine's inbox
 * in-process; in an app of the session it goes to the display's control
 * endpoint; with no display (a lone window, headless) to this engine's own.
 * Everything returns a promise. A failure rejects with an Error whose message
 * is the inbox's reason.
 *
 * Main realm only. Reading bro.inbox the first time compiles it.
 */

/**
 * @typedef {Object} InboxFile
 * @property {string} name   a plain file name (no directory part)
 * @property {Uint8Array|ArrayBuffer|string} data   what you send (a string is its UTF-8)
 *   Files you receive always carry a Uint8Array.
 */

/**
 * Who sent a request: the sender's declarations and the transport's facts.
 * @typedef {Object} InboxSender
 * @property {string} agent      self-declared agent name (ask's `from`, else bro.app.id)
 * @property {string} [origin]   self-declared user@machine
 * @property {string} via        'remote' (broremote's text lane) | 'local' (the control socket) | 'app' (in-process)
 * @property {string} [user]     the login of the process that connected
 * @property {string} [sshClient] the ssh client address whose login started it
 * @property {string} [client]   a remote session's name
 * @property {number} [pid]      the process at the other end
 */

/**
 * A request's state.
 * @typedef {Object} InboxStatus
 * @property {string} id
 * @property {string} to
 * @property {'queued'|'working'|'done'|'failed'|'cancelled'} state
 * @property {InboxSender} from
 * @property {number} asked       unix ms
 * @property {number} elapsedMs
 * @property {string} request     the text asked
 * @property {{name:string,size:number}[]} requestFiles
 * @property {{i:number,atMs:number,text:string}[]} progress   from `since` on
 * @property {number} progressNext   pass as `since` to get only newer lines
 * @property {string} [text]      the reply (done)
 * @property {InboxFile[]|{name:string,size:number}[]} [files]  the reply's files: their bytes from ask/result, names and sizes from status
 * @property {string} [error]     why (failed, cancelled)
 */

const inbox = bro.inbox;

/** The registered agents: [{name, about, by, since, asked, answered, queued, working, polling, idleMs}]. */
inbox.agents(); // → Promise<object[]>

/**
 * Ask an agent. Without `wait` it resolves at once with the request's
 * status (its id). With `wait` (seconds) it resolves when the agent answers,
 * with the reply and its files, or with the status still running when the
 * wait runs out.
 * @param {string} name
 * @param {string} text
 * @param {{wait?: number, files?: InboxFile[], from?: string}} [options]
 * @returns {Promise<InboxStatus>}
 */
inbox.ask('summarize', 'in three lines', { wait: 60, files: [{ name: 'notes.txt', data: 'long notes' }] })
    .then((r) => console.log(r.state, r.text, r.files.map((f) => f.name)));

/** The state and the progress lines from `since`; with `wait` held until there is a newer line or it finishes. */
inbox.status(id, { since: 0, wait: 20 }); // → Promise<InboxStatus>

/** The answer with its files' bytes, waiting up to `wait` seconds for it. */
inbox.result(id, { wait: 60 }); // → Promise<InboxStatus>

inbox.cancel(id); // → Promise<{id, state}>

/** The audit log's last lines, parsed ({t, event, id, to, from, state, text, files}), one request's with `id`. */
inbox.log({ limit: 50, id }); // → Promise<object[]>

/**
 * The inbox's event feed: {next, events: [{seq, at, kind, id, to, from, state, text}]}.
 * kind: 'registered' | 'left' | 'asked' | 'delivered' | 'progress' | 'done' | 'failed' | 'cancelled'.
 */
inbox.events({ since: 0, wait: 20 }); // → Promise<object>

/** Calls fn for every new event until stop(): what helm's notification of agent traffic runs on. */
const watcher = inbox.watch((e) => console.log(e.kind, e.from, '->', e.to, e.text));
watcher.stop();

/**
 * Serve requests under `name` until close(). The handler gets the request
 * ({id, text, from, files: InboxFile[], asked}) and a context with
 * progress(text), `cancelled` and an `oncancel` hook; it returns the reply's
 * text, or {text, files}, or throws to fail the request. A name a live agent
 * holds is refused unless `replace`. The registration lives while the
 * handle polls; it re-registers by itself if it lapsed.
 * @returns {Promise<{name: string, close(): Promise<void>}>}
 */
const agent = await inbox.serve('upper', { about: 'shouts it back' }, async (req, ctx) => {
    await ctx.progress('working');
    return { text: req.text.toUpperCase(), files: [] };
});
await agent.close();

/** Which inbox: '' the session's (the default), 'self' this engine's, or a control endpoint's name.
 *  A named endpoint is another engine's control socket, so reaching it asks `net` (docs/apps.md,
 *  Permissions); a refusal is an Error with code 'EACCES'. */
inbox.endpoint = '';
