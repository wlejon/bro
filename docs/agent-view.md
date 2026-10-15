# The agent view

How an agent operates a bro desktop (helm as the display server, or any bro
app) through text: what it sees, how it acts, and how little context that
costs. The person uses the same session through helm's remote viewer (video,
input, audio); an agent uses its text side. Locally the commands are
`bro-ctl` commands ([agent-control.md](agent-control.md)); over a remote
session they are meant to travel on broremote's text lane (not built yet).

## Why: the baseline

Four small tasks on halo, done by an agent with only screenshots, coordinate
clicks and the shell's own DOM (2026-10-15): calculate a product in the
Calculator, add an alarm in Clock, copy a character from Character Map, then
close a window. It took about 57 calls, 13 screenshots, 9 missteps, and about
5.5k tokens of context, 85% of them images. Every value the agent needed a
picture for was a string inside an app ("69,104", "07:30 Alarm", "SNOWMAN
U+2603", the "Copied" toast). The missteps came from three things:
- a keystroke that went to an unexpected window;
- a screenshot taken before an app's dialog had painted (settling covered only
  the shell's frames);
- not knowing which window was on top.

The view answers those directly: every action's reply is what changed, after
the apps' own frames settled, and the window lines say which is on top and
which has the keyboard.

## What the agent sees

One text view of the desktop. A window is `wN` (`w0` is this process's own
document: the shell under helm, the app itself in a plain bro). An element is
`N.M`: window N, element M. A handle stays the same for as long as its
element exists, so diffs and actions can name it.

```
@view r9
w0 shell "Helm Desktop" 0,0 1920x1080
w3* helm.charmap "Character Map" 420,180 1080x720
w1 broterm "bash" 410,190 1100x700
w0:
 0.26 button "Applications (Super)"
 0.29 button "Character Map"
w3:
 3.6 textbox "Search characters" =""
 3.11 radiogroup
  3.12 radio "Grid View (Ctrl+1)"
 3.56 listbox [20 of 44]
  3.58 option "🌈 U+1F308"
  3.59 option "🌍 U+1F30D"
 3.104 textbox "Copy Buffer" =""
 3.106 button! "Copy Buffer"
 3.120 button "Copy Character"
 3.138 text "F0 9F 8C 88"
```

- **Window lines:** handle with its flags (`*` focused), app id, title in
  quotes, position and size in screen CSS px, then `[...]` notes (`min` for a
  minimized window, `no view` when the app could not be asked). Lines are in
  stacking order, top first; `w0` leads.
- **Element lines:** indent (one space, plus one per level of nesting under
  a container), handle, role with its flags, name in quotes, `="value"` for
  fields, `[...]` notes. Flags: `*` checked, selected, pressed or current
  (a checked `<input>`, `<option selected>`, `aria-checked` true or mixed,
  `aria-selected`, `aria-pressed`, `aria-current`); `!` disabled; `^`
  expanded; `@` has the keyboard. Passwords show as dots. The state is read
  from standard semantics only: a control drawn from divs supplies its own
  `aria-*` (helmapps' `lib/controls.js` does).
- **Runs:** three or more sibling lines of one role and the same flags, with
  only a name each and handles numbered one after another, are one line:
  `1.21-23 button "7" "8" "9"` is `1.21 button "7"`, `1.22 button "8"`,
  `1.23 button "9"` (not with `--rects`, where each carries its own rect).
- **What is shown:** interactive elements (an element with a click listener,
  `tabindex`, or its own `cursor: pointer` counts as a `button`), their
  containers (dialog, list, listbox, menu, tablist, tree, grid, table,
  radiogroup), named groups and regions, and meaningful text (headings,
  images, status, list items, cells, and loose text as `text`). Wrappers
  collapse away; a label is its control's name, and neither it nor any text
  inside it is a line; likewise the title that names a dialog
  (`aria-labelledby`) is the dialog's name, not a line inside it. A live region (status, alert, log, timer, marquee)
  holding controls, such as a toast with its Dismiss button, is named by its
  text, with only its controls under it. Anything not visible is left out:
  `display: none`, `visibility: hidden`, opacity 0, `hidden`, `inert`,
  `aria-hidden`, and whatever is scrolled or clipped out of the viewport or
  its scroll container. A compositor window's frame (`data-window-frame`,
  its title bar and buttons) is left out too: the window's line stands for
  it, and `focus wN` / `close wN` act on it.
- **Terminals:** a `<terminal>` is a `terminal` line (its title, or its
  `aria-label`; `[cwd]` when the program reported one), then a `text` line
  per non-blank row of its screen, trailing blanks trimmed, the last 60 rows
  at most (`[…, last 60 of 84 rows]`). A row's handle is its row's, so a diff
  is row by row: new output on a fresh row is one `+` line, a scroll changes
  the rows that moved. Output counts as a change of the view (and keeps an
  action's settle waiting) though it changes no DOM.
- **Roles and names** come from the same computation as the accessibility
  tree (`dom/accessibility.h`, shared with `src/a11y`): ARIA roles explicit
  or implicit; names from `aria-labelledby`, `aria-label`, a `<label>`,
  `alt`, the content (for roles named from content), `title`, `placeholder`.
- **Scope:** by default the window lines, `w0`'s elements, and the focused
  window's. `view w2` gives that window's instead.
- **Truncation:** names and values (terminal rows included) are cut at 80
  characters (`…`). A list shows its first 20 items and `[20 of 44]`; `view
  3.56 --from=20 [--count=N]` pages it (`[21-40 of 44]`). An element with
  more than 20 controls of one role among its children (a sidebar of 24
  Unicode blocks) is a `group` line paged the same way, showing 12: `group
  [12 of 26]`. A view stops at 400 lines.
- **Handles survive re-renders.** An app that rebuilds a list on every
  change replaces its elements with equivalent ones; a new line standing
  where a line of the last view stood, whose element has left the document,
  takes that line's handle when its role, name and depth agree (or its role
  and depth, between lines that kept theirs). So a re-render that changes
  nothing visible is no change, a held handle stays valid, and a rebuilt row
  whose text changed is one `~` line.
- **Positions:** element rects are left out, since a handle is what an
  action takes; `--rects` adds them (`3.6 textbox "Search" ="" 438,236
  320x32`), in screen CSS px. They are not part of a revision: a window that
  moved changes only its own line.
- **Sources:** a bro app serves its own view over its control endpoint, and
  the display server asks it. A window whose app serves none (not a bro app)
  is its line alone. AT-SPI views of other toolkits' apps are not built.
  Iframes' documents are not included.

## Revisions, diffs and no-change

Every view carries a revision, `rN`, per scope. Clients are stateless: they
ask for the change since a revision they hold, and the server keeps the last
16 views of each scope to compute it. A scope's revision moves only when
what it shows changed. The numbers are shared by all scopes (a revision
names one view of one scope, so one scope's revision never diffs against
another's), so a `view w3` or a paged list between two default views makes
the default scope's next revision skip those numbers.

```
$ bro-ctl set 3.6 snowman
@view r10 +1 -19 ~26
+ 3.176 button "Clear search"
- 3.59-77
~ 3.6 textbox@ ="snowman"
~ 3.54 "1 character"
~ 3.56 listbox [none]
~ 3.58 "☃ U+2603"
~ 3.115 "SNOWMAN"
~ 3.138 "E2 98 83"
...
```

- **Added** items are whole lines after `+ `, a run of them keeping its
  nesting (a dialog's fields indented under the dialog); **removed** handles
  share one `- ` line, handles numbered one after another as a range
  (`2.56-67`), and a window none of whose elements are left (the focus moved
  off it, or it closed) is the one entry `4.*`; a **changed**
  item is `~ ` with its handle and only the fields that changed: role with
  the flags it has now, then `-x` for each flag it lost (`~ 6.6 textbox -@`
  lost the keyboard, `~ w0 -*` lost the focus), name, value, notes (`[none]`
  is notes cleared).
- **No change:** `view --since=r10` answers the single line `@view r10 =`.
- **Expired revision:** a revision too old to diff against gets the full
  view, headed `@view r12 full`.
- `--if-changed=rN` is the same, for the display asking an app.

So a heartbeat costs one line while the desktop holds still.

## Acting

Actions name a handle; the plain forms (`click 120,300`, `type text`, `key
ctrl+l`) still drive the pointer and keyboard as before.

```
click 3.4                       move to its visible centre and click (--button --count --hold)
type 3.1 "snowman"              give it the keyboard, then type (appends: ctrl+end first)
set 3.1 "snowman"               give it the keyboard, select all, type (replaces; "" clears)
key 3.1 enter | key w3 escape   give it the keyboard, then the combos
scroll 3.2 3                    wheel detents over it (+ is down)
focus w2 | close w2             raise and focus a window, close it
launch helm.calculator          start an app; waits up to 8 s for its window
```

- "Give it the keyboard": its window is focused first if it is not; an
  element is clicked unless it already has `@`, since a second click on a
  focused field would read as a double click.
- **Real input:** under helm the clicks and keys go in at the device level
  through the seat, through compositor focus, exactly as the person's do.
- **The reply is the diff.** An action takes the view before it (the default
  scope plus the target's window) to find its target, acts, waits until
  everything settles, and answers with the diff since the default scope's
  last revision given out (the one the agent holds), or since `--since`.
  Settled is the display and every visible app idle for 3 frames, the apps
  asked in parallel (`wait-idle --settle=300`), with the follow-on work the
  action set off done: every script timer due within 300 ms of it has run (a
  re-render the click scheduled, a 250 ms tick that redraws a count), no
  promise job waits, and no terminal's screen is still changing.
  A dialog that opens, a toast, or a value that changes is the action's
  answer, so acting and looking are one round trip. A plain form with
  `--diff` answers the same way.
- A handle that is gone answers `no 3.4 in view (gone, hidden or scrolled
  away)`.

## Pictures, when wanted

```
shot 3.120 [path] [--scale=0.5] | shot w3 | shot [path]
```

A PNG of an element, a window, or the screen. An app's window and elements
come from the app's own screenshot, so a covered window still shows. With
`bro-ctl -o FILE` (or `-o -`) the PNG is written where bro-ctl runs.

## Supporting commands

- `windows`: the window lines alone.
- `who-has-keys`: the focused window's line and the element with `@`.
- `clipboard [--local]`: the clipboard's text. A display asks the focused app,
  since the clients' selection is not the compositor's own clipboard.
- Not built: `hotkeys` (the shell's global shortcuts).

## How it is built

- **Every bro process serves its own view** (`src/engine/view_document.*`
  walks the document; `agent_view.*` holds the line format, diffs and
  revision history, pure and unit-tested; `control_view.cpp` the commands).
  It runs only when asked. The walk is cached by the document's mutation
  epoch, focus, viewport and scroll, so asking again of an unchanged app is a
  lookup.
- **Apps in a session serve control.** A display running its own compositor
  (helm under `bro --drm`) sets `BRO_CONTROL_SESSION=1` for what it starts;
  a windowed app started any other way (a systemd unit, an app under the
  Windows desktop shell) serves too when the `display` endpoint is up as it
  starts. The endpoint is `<app>-<pid>`, so
  the display finds a window's app by the window's process id. Serving
  control costs no frame readback: a windowed bro reads back a present only
  while a screenshot or a recording waits for one.
- **The display aggregates.** Window lines come from the compositor (stacking
  order, focus, frames); `w0` is its own document; each visible window's
  elements are fetched from its app on a worker thread (2 s timeout), the
  app's `0.M` renumbered `N.M` and its rects placed on the screen. Window
  numbers stay with a window for its life.
- **Elsewhere** (a plain windowed bro, `bro-headless`) the same commands
  serve the one document as `w0`; the menu bar's inset is accounted for, so
  rects and clicks are window coordinates.
- **Remote sessions:** broremote's text lane is to carry these commands, so
  an agent in a remote session needs no ssh login of its own.

## Costs

Measured on halo (helm under DRM), through `bro-ctl` over the local socket:

- `info` to the display: 1 ms round trip. `view` of the desktop with
  Character Map focused (116 lines, 3.4 KB): 33 ms. `view --since` with
  nothing changed: 32 ms, 12 bytes. Most of that is waiting on the frame
  loops (the display's, then the idle app's, about 15 ms for any command),
  not the walk.
- A click's or `set`'s diff: 100-700 bytes; a calculator key press about
  120 bytes.
