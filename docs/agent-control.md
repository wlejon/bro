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

## Security

The channel is [brolink](https://github.com/wlejon/brolink)'s local IPC,
the transport bromux and `bro.remote` use. On Linux and macOS the socket is
`$XDG_RUNTIME_DIR/bro-control/<name>.sock` (without `XDG_RUNTIME_DIR`,
`${TMPDIR:-/tmp}/bro-control-<uid>/`): a directory only its owner can enter
(0700, and refused if it is anything else), a socket only its owner can open
(0600), and the peer's uid checked on every connection, by the server and by
bro-ctl alike (SO_PEERCRED / getpeereid). It is never a network listener:
reaching it from elsewhere means logging in as that user first (ssh), and
anyone who can do that can already run anything as them.

On Windows it is a named pipe, `\\.\pipe\bro-control-<user SID>-<name>`,
whose DACL admits only that user and which refuses remote clients; bro-ctl
checks that the process serving it runs as the same user before it speaks.
Files a command writes for bro-ctl (`-o -`, a default screenshot or record
path) go to `%LOCALAPPDATA%\bro-control\`.

## When it is on

- `bro --drm` (bro as the display server) serves `display`, always.
- Elsewhere (windowed, headless) only when asked: `BRO_CONTROL=1` serves
  `<app>-<pid>`; `BRO_CONTROL=<name>` picks the name.
- `BRO_CONTROL=0` turns it off everywhere.

The pid is in the name on every OS, so any number of bros (two windows of one
app included) serve side by side. A live server keeps its name: a second bro
asking for it is refused. A bro killed without removing its socket leaves the
file behind (POSIX; a pipe goes with its process), and the next bro to start
control removes every such socket in the directory, whatever its name: a
server holds a lock file beside its socket while it serves, so a lock anyone
can take marks a dead one. `bro-ctl list` shows the endpoints and which
answer. `-s NAME` picks one: the whole name (`helmterm-4242`), the app alone
(`helmterm`, when one process of it is running; with several, bro-ctl lists
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

`bro-ctl` adds three of its own:

- `batch` reads commands from stdin, one per line (shell-style quoting, `#`
  comment lines, `sleep <secs>`), on one connection, echoing each.
- `profile <secs> [--top=40]` samples the process with `perf` (installed
  separately: `pacman -S perf`, `apt install linux-perf`) and prints the
  hottest symbols and threads. On Windows, `bro.profiler` profiles script.
- `-o FILE` (or `-o -` for stdout) on `screenshot` and `record` writes the
  result where bro-ctl runs; `eval -` / `eval -f FILE` read the script from
  stdin or a file.

Input goes in at the device level: under DRM through the same path libinput
events take (so global hotkeys, the pointer, and client windows all see it);
elsewhere through the engine's input handlers. It is played out on a
timeline (a click is a move, a press, 40 ms, a release) one frame at a time,
and the command replies once it has all been delivered.

## record

```
bro-ctl record 3 /tmp/rec [--scale=0.5]
```

Under DRM, every frame the display presents for 3 s is copied off the
scanout image on the GPU (scaled), with the kernel's flip timestamp and
vblank counter. A windowed bro (Windows, macOS, a Wayland or X11 client)
records each frame its presenter read back from the swapchain (asking for
the socket with `BRO_CONTROL` keeps that readback on), stamped with the time
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

## Where it lives

| | |
|---|---|
| `src/platform/control_socket.*` | the server on brolink's event loop: requests in, replies out |
| `src/platform/control_protocol.h` | the messages, shared with bro-ctl |
| `src/engine/control.*` | the command registry and per-frame pump (no socket details) |
| `src/engine/control_commands.cpp`, `control_input.cpp`, `control_record.cpp` | engine commands |
| `src/bronze_host/host_control.*` | `eval`, `dom`, `inspect`, `style`; the headless globals |
| `src/engine/frame_trace.*`, `engine_frame_trace.cpp` | the flight recorder |
| `src/render/scanout_capture.*` | the recorder's GPU frame tap on the KMS presenter |
| `src/ctl/bro_ctl.cpp` | the client |

The transport is [brolink](https://github.com/wlejon/brolink)'s local IPC
(listening, peer checks, stale-endpoint cleanup) and its message framing
(`u32` length, `u16` type, body). A `Request` (type 1) is `varint id,
strings argv`; a `Reply` (type 2) is `varint id, bool ok, str payload`. A
connection may carry several requests, and each reply names the request it
answers. Nothing in it depends on the local socket, so any brolink stream (a
remote session's lane) can carry the same protocol.
