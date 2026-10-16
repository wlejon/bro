# Agent inbox: agents asking each other through a session

An agent running *inside* a session (a model process, a Claude Code
instance on halo, a shell script) registers in the session's inbox under a
name, with a line saying what it does, and takes requests. Other agents,
in the session or on another machine through the remote, list the agents
and ask them things: text and files in, text and files back, waited for or
followed by id. Every request says who sent it, every step lands in an
audit log on the host, and helm shows the traffic to the person watching.

This is the transport and the contract only. It does not include any agents.

```bash
# on halo, in the session: an agent
bro-ctl inbox serve summarize --about "summarizes text files" --exec ./summarize.sh

# from Windows, through the remote
bro-ctl --remote halo agents
bro-ctl --remote halo ask summarize "the attached, in 3 lines" --file notes.txt
id=$(bro-ctl --remote halo ask summarize "the long one" --file big.txt --async)
bro-ctl --remote halo inbox status $id --follow    # progress lines as they come
bro-ctl --remote halo inbox result $id             # the reply, files saved under ./$id/
bro-ctl --remote halo inbox log                    # who asked what, and what came back
```

## Where it lives

The inbox is part of the engine's agent control dispatcher
([agent-control.md](agent-control.md)): its commands are `agents`, `ask` and
`inbox <sub>`, plain control commands. So they reach every way control
does: a local `bro-ctl`, an app's `bro.inbox`, `bro-ctl --remote HOST` and
a viewer's `session.request()` over broremote's text lane. Nothing in the
wire protocols is specific to it. Every engine has an inbox. The session's
inbox is the display's (helm's engine under `--drm`), which is what
`bro-ctl` reaches by default and what `--remote` reaches. An app's
`bro.inbox` uses the display's inbox when one serves.

Everything is request and reply. A serving agent long-polls for its next
request rather than holding a stream open, so the inbox crosses
the remote exactly as a screenshot does, files included: files go to the
engine as a `--files=` argument holding a bundle (`src/platform/inbox_bundle.h`:
a count, then name and bytes per file; 48 MB at most), and come back
through an output-path argument, which a remote client names as its
`outArg` so the bytes come back in the reply.

## Serving: `bro-ctl inbox serve`

```
bro-ctl [-s NAME | --remote HOST] inbox serve NAME [--about TEXT] [--replace] [--dir DIR] [--exec CMD]
```

This registers NAME (1 to 64 of `A-Z a-z 0-9 . _ -`) and serves it until
stdin ends, or until SIGINT or SIGTERM, then unregisters. A name that a live
agent holds is refused unless `--replace` is given. A replacement takes over
the requests still queued, and requests its predecessor was working on
fail. Each request's files are written to
`DIR/<id>/files/` (default `DIR` is `<control runtime dir>/inbox/NAME`) beside
`request.json`.

**JSON lines (the default).** Each request is one line on stdout:

```json
{"type":"request","id":"9f2a-3","text":"the attached, in 3 lines",
 "from":{"agent":"claude-win","origin":"jonny@JDESK","via":"remote","user":"j",
         "sshClient":"10.1.0.4","client":"bro-ctl agent (pid 4120)","pid":31337},
 "files":["/run/user/1000/bro-control/inbox/summarize/9f2a-3/files/notes.txt"],
 "dir":"/run/user/1000/bro-control/inbox/summarize/9f2a-3","asked":1760600000000}
{"type":"cancel","id":"9f2a-3"}
```

Answers go on stdin, one JSON object per line:

```json
{"id":"9f2a-3","progress":"read 2 of 5 files"}
{"id":"9f2a-3","text":"the summary...","files":["/tmp/summary.md"]}
{"id":"9f2a-3","error":"could not read notes.txt"}
```

A line with `progress` adds a progress line. A line with `text` (and
optionally `files`, paths to attach) answers. A line with `error`, or with
`"ok":false`, answers as failed. Requests may be answered in any order, and
several may be in flight. Any harness can wrap this: a `coproc` in bash, a
subprocess in Python, an LLM loop.

**A command per request (`--exec CMD`).** For each request, CMD runs
through the shell (`sh -c`, or `cmd /c` on Windows) in the request's
directory, with the request text on stdin and `BRO_INBOX_ID` set. Its
stdout is the reply. Each line it writes to stderr becomes a progress line
as it is written. Files it leaves in `./out/` are attached. A nonzero exit
fails the request with the tail of stderr. `./request.json` and
`./files/` hold the rest. Requests run concurrently, each in its own
directory. A cancel does not stop a running command; its reply is refused.

```bash
#!/bin/sh
# summarize.sh: progress on stderr, the answer on stdout, files in ./out
echo "reading $(ls files | wc -l) files" >&2
cat files/* | head -c 2000 > out/excerpt.txt
wc -w files/*
```

The serve process keeps its registration alive by polling: an agent whose
last poll ended more than 45 s ago lapses, and the requests it held fail
("stopped polling"). If the registration lapsed while the serve process
still ran (the machine slept), serve registers again on its next poll.

## Asking: `bro-ctl ask`

```
bro-ctl [--remote HOST] agents [--json]
bro-ctl [--remote HOST] ask NAME TEXT... [--file PATH]... [--from NAME] [--wait SECS] [--async] [--save DIR] [--json] [--quiet]
bro-ctl [--remote HOST] inbox status ID [--follow]
bro-ctl [--remote HOST] inbox result ID [--wait SECS] [--save DIR] [--json]
bro-ctl [--remote HOST] inbox cancel ID
bro-ctl [--remote HOST] inbox log [--limit N] [--id ID] [--json]
```

- `ask` sends the request (TEXT `-` reads stdin), then follows it, printing
  each progress line to stderr (`[NAME] text`), until it finishes or
  `--wait` (default 600 s) runs out. It prints the reply's text on stdout and
  saves its files under `--save DIR` (default `./<id>/`), naming each on
  stderr. Exit status: 0 done, 1 failed or cancelled, 2 no connection,
  3 still running (the id is printed: pick it up with `inbox result`).
- `--async` prints the id and returns at once.
- `inbox status ID` prints the request's state as JSON. `--follow` streams
  its progress lines until it finishes.
- `--json` prints the engine's JSON (with `saved`, the paths written).
- `--from NAME` is the name this asker declares (default
  `$BRO_AGENT_NAME`, else `user@machine`).

## Identity

Every request carries who sent it, in two halves:

| field | from | what |
|---|---|---|
| `agent` | the sender | its self-declared agent name (`--from`) |
| `origin` | the sender | its self-declared `user@machine` (bro-ctl fills it in) |
| `via` | the transport | `remote` (a broremote session's text lane), `local` (the control socket), `app` (in-process) |
| `user` | the OS | the login of the process that connected: the host's own user, always |
| `sshClient` | the OS | the address of the ssh client whose login started that process (`SSH_CONNECTION`; Linux), empty for a program of the local session |
| `client` | the session | a remote session's name (its `Hello`): `bro-ctl agent (pid N)` |
| `pid` | the OS | the process at the other end: for `remote`, the `broremote proxy` sshd started |

The transport half comes from the connection, not from the request: the
control socket and the broremote server ask the kernel for the peer's pid
(`brolink::EventLoop::peer_pid`) and read its ssh session from its
environment (`brolink::describe_process`). It tells you what kind of
access the request came through, but it is not a credential. The declared
half is whatever the sender says.

## The audit log

The audit log is one JSON object per line, appended by the engine that
holds the inbox: `$BRO_INBOX_LOG`, else `<user data>/agent-inbox.jsonl`
(`~/.local/share/bro/agent-inbox.jsonl` on Linux,
`%APPDATA%\bro\agent-inbox.jsonl` on Windows). It moves to `.1` past 8 MB.
Events: `register`, `leave`, `ask` (with the sender and the files' names and
sizes), `deliver`, `progress`, `reply`, `fail`, `cancel`. Each event has `t`
(unix ms), the request's `id`, `to`, `from` and `state`, and up to 4000
bytes of text. `bro-ctl inbox log` prints them readably (`--json` raw), and
`--id ID` prints one request's whole life:

```
2026-10-16 14:02:11  register  summarize registered (summarizes text files) via local pid 2214
2026-10-16 14:02:30  ask       9f2a-3  claude-win (jonny@JDESK; remote j from 10.1.0.4) -> summarize: the attached, in 3 lines  [notes.txt]
2026-10-16 14:02:30  deliver   9f2a-3  summarize
2026-10-16 14:02:31  progress  9f2a-3  summarize: read 1 of 1 files
2026-10-16 14:02:33  reply     9f2a-3  summarize: the summary...  [summary.md]
```

## In a page: `bro.inbox`

`bro.inbox` ([inbox-api.js](inbox-api.js)) is the same contract for a bro
app: `serve(name, {about}, handler)`, `agents()`, `ask(name, text, {wait,
files})`, `status`, `result`, `cancel`, `log`, and `watch(fn)` for the event
feed. In the display's own realm (helm's UI) it is in-process. In an app of
the session it reaches the display's endpoint.

## What the person sees

helm shows the traffic (helm's `ui/js/agents.js`): an incoming request rises
as a notification ("claude-win asks summarize") and stays as a live activity
in the clock island while it is open, ending with a toast saying done or
failed. The engine's event feed (`inbox events --since=N --wait=SECS`,
`bro.inbox.watch`) drives it. A person watching through a remote viewer sees
this as it happens, and an agent sees the same toasts in `bro-ctl view`.

## Limits

| | |
|---|---|
| text (request or reply) | 1 MB: anything bigger is a file |
| files per message | 48 MB together, 4096 files, plain names (no directory part) |
| queue per agent | 1000 undelivered requests |
| kept for status/result | the last 256 finished requests (256 MB of files) |
| lease | 45 s after the agent's last poll; a poll is held at most 25 s |

A request handed to a serving agent that died before it answered fails
when the agent's lease lapses, not sooner. The engine does not see a client
connection go.

## Authorization

The session is the credential, as for every control command: reaching the
inbox takes this user's local socket, or an ssh login as this user through
the remote. Whoever can do that can ask any agent anything, and serve any
free name. There is no second check today.

The seam for one is `AgentInbox::admits(sender, agent)` (`src/engine/agent_inbox.cpp`),
which every `ask` passes through and which now admits everything. Per-agent keys and
capabilities would sit there:

- **Keys.** An asker would present a key id (an `ask --key=`), checked
  against keys the host trusts (`~/.config/bro/agent-keys`, an ssh-style
  list of public keys with names). The key would be proved by signing the
  request id with a nonce the inbox issues at `register` or per session.
  The audit log would then record the key's name as well as the
  transport's facts.
- **Capabilities.** An agent would declare what it accepts at `register`
  (`--accepts=summarize,files`), and a key would carry what it may ask
  (`claude-win: summarize, review`). `admits` would refuse a request outside
  both, with the reason, and log the refusal.

Neither changes the wire contract: both are options on commands that exist.

## Broadcast across machines (design, not built)

The goal is a few halo-type boxes on one LAN (four or five), each running
helm with agents in its inbox, where an agent on any box (or on Windows)
broadcasts a request that any capable agent on any box may answer. Here is
how it would sit on this contract.

**Discovery.** Each box announces its inbox on the LAN with DNS-SD
(`_bro-inbox._tcp`, TXT: host name, the ssh user, broremote socket, the
names and abouts of its agents, a change counter), through avahi on Linux.
The announcement only names a box. It grants nothing. Reaching a box is
what reaching it is today: an ssh login as its user, through broremote's
text lane (a `bro-ctl --remote BOX` agent process per box, already one ssh
connection each). A box list in `~/.config/bro/inbox-peers` (host names
the user has ssh keys for) is the fallback where mDNS is not wanted, and
the allowlist where it is: a discovered box not on the list is shown, not
used.

**Fan-out.** A broadcast is a client-side operation over the per-box
contract, not a new transport:
`bro-ctl ask --broadcast CAPABILITY TEXT [--first | --all] [--wait SECS]`
would:
1. `agents --json` on every known box (in parallel, over each box's
   agent connection), and pick the agents whose `about` (or, with
   capabilities, whose declared `accepts`) matches;
2. `ask --async` each of them, recording `<box>/<id>` per request;
3. follow them with `inbox status --wait` on each box. `--first` takes the
   first `done` and `inbox cancel`s the rest. `--all` gathers every reply,
   with the box and agent named on each.

A broadcast has an id of its own (`b-<random>`), carried on each request
as `--broadcast=ID`, which the engine would add to the request's sender
fields and the log. Each box's audit log then shows the same broadcast id,
and the logs can be joined without a central server.

**Identity across boxes.** Each hop keeps the facts it can see. Box A's
inbox logs the asker's transport (`remote`, `j` from Windows'
address). When the fan-out runs on box A (an agent there broadcasting), box
B logs `remote` from A's address with A's declared `origin`. So "who" is
the chain of declared `origin`s, each hop verified only as far as the ssh
login it came through. With keys (above), the original asker's signature
would travel with the request, so box B could check the original asker and
not just the box in between.

**Answering.** Nothing changes for a serving agent: a broadcast request is
an ordinary request in its inbox (the `from` names the broadcast), answered
the ordinary way. An agent that is not capable simply is not asked, because
the asker filtered on `about` / `accepts`. A later `--accepts` makes that
precise instead of a text match.

**What would be built:** the DNS-SD announcement (a small service in
bro, started where helm hosts the remote), the peers file, `ask
--broadcast` in bro-ctl (and `bro.inbox.broadcast` over the same per-box
calls from a page, once a page can hold remote sessions of its own), and
the `--broadcast` sender field.

## Headless tests

`tests/headless/test_agent_inbox.js` drives the commands in-process
(`controlCommand`), held polls and the lease on virtual time, and
`tests/headless/test_inbox_js.js` drives `bro.inbox`.
