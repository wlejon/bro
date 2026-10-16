# Agent control: observing and driving a running bro

A running bro serves a local control socket that a tool (an AI agent, a
script, a person at a shell) uses to see what is on screen, drive input, read
the DOM and computed styles, run JavaScript in the page, and find out why an
animation stuttered: screenshots, short full-rate recordings with the vblank
each frame landed on, a frame-timing flight recorder, and a native profile.

`bro-ctl` is the client. It is built beside `bro` (target `bro-ctl`; it links
brolink and nothing of bro's) on Linux, macOS and Windows.

```bash
bro-ctl info                                  # what is running
bro-ctl screenshot shot.png --scale=0.5       # the screen, half size
bro-ctl click '#island-center'                # move there and click
bro-ctl record 3 /tmp/rec                     # 3 s of every presented frame
bro-ctl trace 5                               # pacing over the last 5 s
```

From another machine, through ssh, with nothing to set up on either end:

```bash
ssh box bro-ctl -o - screenshot --scale=0.5 > shot.png
ssh box bro-ctl eval 'document.title'
printf 'key super+s\nsleep 1\nkey escape\n' | ssh box bro-ctl batch
```

## Remote: a session's engine over broremote

`--remote HOST` sends the same commands to the engine of a remote session
(helm under `helm --drm --remote`, or any bro hosting with `bro.remote.host`)
through broremote's text lane (protocol 1.6), not a shell on that machine.
The replies are the same text, and the remote engine's dispatcher runs every
command, so nothing below is local-only except `profile` and `record -o`.

```bash
bro-ctl --remote halo windows
bro-ctl --remote halo view
bro-ctl --remote halo launch helm.calculator
bro-ctl --remote halo -o w4.png shot w4     # the PNG comes back in the reply
export BRO_CTL_REMOTE=halo                  # then plain `bro-ctl view` goes there (--local: here)
```

- **The agent.** The first command starts a local agent (a `bro-ctl` in the
  background) that holds one broremote session to the host: one ssh login,
  then every command is a local connection to the agent and one round trip
  on the open lane. It serves this user only (brolink application
  `bro-ctl-remote`, an endpoint named for the host), ends after 30 minutes
  with nothing asked (`--idle SECS`) or when its session ends, and
  `bro-ctl --remote HOST disconnect` stops it (`connect` starts it and says
  how it is). `--direct` skips it: one ssh per command, nothing left
  running. `--remote-socket NAME` names the host's broremote socket
  (default `default`, what helm uses).
- **Beside a person's viewer.** The agent's session is text-only: it is sent
  no video, is not counted as a viewer (no `attach` event, no encoder, no
  frames captured for it), and changes nothing for a person watching through
  `remote` / `<remoteview>`: their video and input lanes stay theirs, and
  what the agent does (launching, typing) they see happen. With no viewer at
  all, nothing is encoded. A viewer app can send the same commands over its
  own session's text lane (`session.request()`, docs/remote-api.js).
- **Files.** With `-o FILE` (or `-o -`), `screenshot` and `shot` write on
  the host into a private file of the engine's, whose bytes come back in the
  reply and land in FILE here; the reply text names FILE. Without `-o` the
  file stays on the host (the reply says where). `record` writes on the host
  only.
- **Authorization** is the session: reaching the host's broremote server
  takes an ssh login as the host's user (or that user's local socket), and
  that is all. Whoever can log in as that user can drive the session
  entirely, input, `eval` and `launch` included, exactly as they could with
  a shell running `bro-ctl` there; there is no second check, so grant ssh
  access to the host user accordingly.
- **What it costs** (Windows to halo on a LAN with sub-millisecond ping,
  helm under DRM, Release builds): over the agent a round trip is about
  1 ms (40 commands in one `bro-ctl batch`: 92 ms), so a single command
  costs bro-ctl's own process start, about 45 ms on Windows. `--direct`
  adds an ssh login to each command, about 200 ms (`windows` 240 ms
  against 45 ms). Commands that wait for the UI cost the same either way
  plus that overhead: `launch helm.calculator` 0.5 s, `set` 0.32 s, `key`
  0.23 s, `close` 0.18 s, `shot w1 -o` 0.13 s for a 53 KB PNG. With a person's
  hevc viewer attached the numbers are the same, within a few ms.

## Security

Local IPC only, never a network listener; only the same user can connect
(reach another machine's over ssh, or through broremote's text lane above).

- Linux/macOS: `$XDG_RUNTIME_DIR/bro-control/<name>.sock` (without
  `XDG_RUNTIME_DIR`, `${TMPDIR:-/tmp}/bro-control-<uid>/`); the directory must
  be 0700.
- Windows: the named pipe `\\.\pipe\bro-control-<user SID>-<name>`. Files a
  command writes for bro-ctl (`-o -`, a default screenshot or record path) go
  to `%LOCALAPPDATA%\bro-control\`.

## When it is on

- `bro --drm` (bro as the display server) serves `display`, always.
- Elsewhere (windowed, headless) only when asked: `BRO_CONTROL=1` serves
  `<app>-<pid>` (`<app>` is the app's id: its manifest's, else its folder's
  name); `BRO_CONTROL=<name>` picks the name.
- The apps of a session whose display server runs its own compositor (helm
  under `bro --drm`) serve `<app>-<pid>` without asking: the display sets
  `BRO_CONTROL_SESSION=1` for the processes it starts, and finds an app's
  endpoint by its process id ([agent-view.md](agent-view.md)). A windowed
  app started some other way (a systemd unit) serves too when the
  `display` endpoint is up as it starts.
- `BRO_CONTROL=0` turns it off everywhere.

Serving control costs nothing per frame. A windowed bro reads back none of
its presents until a command needs one: a screenshot turns the readback on
for the one present it waits for (an idle window presents again for it), a
recording for its length.

A second bro asking for a live server's name is refused. Stale sockets from
killed bros are removed when the next bro starts control. `bro-ctl list` shows the endpoints and which
answer. `-s NAME` picks one: the whole name (`term-4242`), the app alone
(`term`, when one process of it is running; with several, bro-ctl lists
them and asks), or the pid (`4242`). Without `-s` the default is `display`,
else the only live endpoint. `-S PATH` names an address directly (a socket
path, or a pipe name on Windows), as does `$BRO_CONTROL_SOCKET`.

## Commands

`bro-ctl help` lists the running engine's commands with their usage; this is
the same list. Targets are `x,y` / `x y` in CSS px or a CSS selector (its
first match's centre).

| Command | What |
|---|---|
| `info` | mode, app, viewport, refresh period, frame number, pid, socket |
| `screenshot [path] [--selector=S [--pad=8]] [--region=x,y,w,h] [--scale=F]` | PNG of the screen (the KMS scanout buffer under DRM), cropped and scaled |
| `record [secs=3] [dir] [--scale=0.5] [--max-mb=3072] [--no-video]` | every presented frame for a while (DRM or windowed): see below |
| `trace [secs=5] [--since=ms] [--until=ms] [--json] [--frames] [--worst=N]` | the flight recorder's pacing summary, or (`--frames`) every frame record |
| `mark <label>` | a labelled mark in the trace; `now` prints the trace's clock |
| `animations [--all]` | running animations and transitions: element, properties, time |
| `move <target> [--ms=0]` | move the pointer, gliding over `--ms` |
| `click [target] [--button=] [--count=] [--hold=] [--ms=]` | move there and click |
| `down` / `up [target] [--button=]` | press or release a button |
| `drag <from> <to> [--ms=400]` | press, glide, release |
| `wheel <dy> [dx] [--at=target] [--steps=N]` | scroll in wheel detents (+dy is down) |
| `key <combo>... [--hold=30] [--gap=40]` | `super+space`, `ctrl+shift+t`, `escape`, `f5` |
| `type <text> [--delay=12]` | ASCII text, US layout |
| `eval <js> [--timeout=10000]` | run in the page's realm; prints the completion value (JSON unless a string; elements as `<tag#id.class>`); a promise is awaited; a throw is an error with its stack |
| `dom [selector=body] [depth=4]` | the layout tree: tag, size, position |
| `inspect <selector> [--verbose]` | box model and computed style of the first match |
| `style <selector> [property...]` | computed style as JSON |
| `rect <selector>` | client rects of every match (CSS px) |
| `wait <selector> [--gone] [--timeout=5000]` | until a match has a box (or none does) |
| `wait-idle [--timeout=5000] [--frames=3]` | until N frames in a row lay out, animate and render nothing |
| `mem [--detail] [--trim]` | what the process costs: see below |

Coordinates are window CSS px; a selector's point is its element's centre
in the window (below a menu bar, if the app shows one).

The agent view's commands, which see and act through element handles
instead of pixels (`view`, `windows`, `set`, `scroll`, `focus`, `close`,
`launch`, `shot`, `who-has-keys`, `clipboard`, and the handle forms of
`click`, `type` and `key`), are in [agent-view.md](agent-view.md).

`bro-ctl` adds three of its own:

- `batch` reads commands from stdin, one per line (shell-style quoting, `#`
  comment lines, `sleep <secs>`), on one connection, echoing each.
- `profile <secs> [--top=40]` samples the process with `perf` (installed
  separately: `pacman -S perf`, `apt install linux-perf`) and prints the
  hottest symbols and threads. On Windows, `bro.profiler` profiles script.
- `-o FILE` (or `-o -` for stdout) on `screenshot`, `shot` and `record`
  writes the result where bro-ctl runs. With `-o -`, stdout carries only
  the file's bytes: every text reply goes to stderr, so a PNG stream stays
  clean. `eval -` / `eval -f FILE` read the script from stdin or a file.

Input goes in at the device level: under DRM through the same path libinput
events take (so global hotkeys, the pointer, and client windows all see it);
elsewhere through the engine's input handlers. It is played out on a
timeline (a click is a move, a press, 40 ms, a release) one frame at a time,
and the command replies once it has all been delivered.

## mem

What this process costs and who holds it, so a footprint can be re-checked
and every megabyte named:

- `process`: resident, private (dirty) and shared memory (Linux:
  `/proc/self/smaps_rollup`; Windows: working set and private bytes).
- `engine image` (Linux): the file holding the engine's code (libbro.so, or
  the executable when the engine is linked in statically), what of it is
  resident and how much is shared clean with other processes mapping the same
  file; its code pages are shared by every bro process and helm on one libbro.
- private dirty memory by kind of mapping (Linux): the C heap's main heap
  and its thread arenas, file-backed private pages (relocations, `.data`),
  GPU buffer mappings, and the anonymous rest (the JS heap, JIT code, thread
  stacks, drivers' own allocations).
- `threads`, grouped by name (`Worker x30` is one family).
- `malloc` (glibc): bytes in use, free bytes held in arenas, arena count.
- `gpu`: VRAM and GTT the kernel charges this process (DRM `fdinfo`).
- `js`: bronze's heap, used / committed / reserved, and collections so far.
- `mir`: the compiled script functions' MIR bodies still held, and how many
  were built and released. An idle window releases every body whose code
  exists; a tier-up or a deopt builds one again.
- `skia` / `ganesh`: the glyph cache, Ganesh's resource cache against its
  budget, the device memory pool.
- `images`: the decoded-image store and its decoder threads.
- `subsystems`: which of physics, audio, audio inference, net and Steam are
  running; an app that never uses one should see it `off`.

`--detail` adds private memory by file and the largest anonymous mappings;
`--trim` first releases the idle MIR bodies and returns the C heap's free
pages to the system (`malloc_trim`)
and says how much that was, which separates fragmentation from live data.

## record

```
bro-ctl record 3 /tmp/rec [--scale=0.5]
```

Under DRM, every frame the display presents for 3 s is copied off the
scanout image on the GPU (scaled), with the kernel's flip timestamp and
vblank counter. A windowed bro (Windows, macOS, a Wayland or X11 client)
records each frame its presenter read back from the swapchain (the readback
is on for the recording's length only), stamped with the time
the frame loop saw the present: the system compositor owns the vblank there,
so `seq` is 0 and gaps are measured against the refresh period (the window
system's, else the display's mode, else the median frame interval). A
window draws only when its content changes, so an idle app records few
frames. Either way it is written out:

- `frames/NNNNN.png` and `frames.ffconcat` (true per-frame durations);
  `video.mp4` when ffmpeg is on the path (bro-ctl runs it, through cmd.exe
  on Windows and sh elsewhere; `--no-video` skips)
- `contact.png`: a sheet of up to 48 frames around the motion, cropped to
  where the picture changed, each labelled with its time; a frame shown
  after a missed vblank is outlined amber, a stall (a frame identical to the
  one before it, between frames that moved) red
- `timeline.json`: per frame its vblank time and sequence, the gap since the
  last (in refresh periods), the fraction of pixels that changed and where;
  the flight recorder's summary and frame records for the same window
- `summary.txt`: the same, short

Recording costs a blit per frame on the GPU and a PNG encode per frame on
worker threads, so it does not itself disturb the pacing it measures.

## The flight recorder (trace)

Every trip round the frame loop leaves a record in a ring (8192 frames, about
two minutes at 60 Hz), always on, so a stutter can be examined after the
fact. A record holds:

- main-thread phases (ms): `events` (transition/animation events), `control`
  (agent commands and their input), `misc`, `input` (libinput, seat, client
  windows), `tick` (timers, rAF, observers; `js` of it), `layoutWait`,
  `record`, `composite`, `present` (with `gpuWait` and `flipWait`), and
  `pacingWait`; `other` is what none of them account for
- `forcedLayouts` / `forcedLayoutMs`: layout forced on the main thread by a
  script reading geometry after a mutation
- the layout pass it waited on (`style`, `layout`, `animTick`, active and
  promoted animations) and the raster pass it showed (`raster`)
- `contentGen` / `contentTime`: which frame's content it showed, and the
  animation clock that content was laid out at
- under DRM, the vblank it flipped on (`vblank` ms, `vblankSeq`)

`bro-ctl trace` summarizes a window of them: presents and new content per
second; vblank gaps; **stalls** (content held mid-motion); **judder** (how far
each step the animation took differs from the step the display took: zero
when every frame shows the animation exactly one refresh further on); phase
means, p95 and max; and the longest frames broken down by phase. `--json`
gives the same as JSON, `--frames` every record.

To measure one interaction, bracket it: `t0=$(bro-ctl now)`, act,
`bro-ctl trace --since=$t0`; or `record` while a `batch` plays.

## Headless tests

The same commands run in-process in `bro-headless`, without a socket:
`controlCommand(...argv)` returns an id and `controlResult(id)` is `null`
until the reply, then `{ok, payload}`. Input timelines play out against the
virtual clock, so advance time while waiting
(`tests/headless/test_agent_control.js`).
