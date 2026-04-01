# bro-headless

A headless mode for bro that loads an app without any window or GPU and lets you inspect and manipulate the DOM via text commands. Built for automated testing and debugging.

## Usage

```
bro-headless <app-directory> [script.txt]
```

- No script argument = **interactive mode** (reads from stdin)
- With script argument = **script mode** (runs commands from file, then exits)
- Piping works: `echo "dump #btn" | bro-headless apps/hello`

## Commands

| Command | Description |
|---------|-------------|
| `dump` | Print full DOM as HTML |
| `dump <selector>` | Print a single element's outer HTML |
| `diff` | Show line-by-line diff since last `dump` |
| `click <selector>` | Simulate a click event on the element. Prints `[changed]` if the DOM was modified |
| `eval <js>` | Evaluate JavaScript and print the result |
| `wait <ms>` | Advance virtual time by N milliseconds (fires pending timers) |
| `help` | Print command reference |
| `quit` / `exit` | Exit |
| `# comment` | Ignored (for script files) |

Selectors: `#id` for getElementById, or any CSS selector supported by htmlayout.

## Examples

### Interactive

```
> bro-headless apps/hello

bro headless> dump #counter
<div id="counter"></div>

bro headless> click #btn
[console.log] Button clicked, count: 1
[changed]

bro headless> dump #counter
<div id="counter">Count: 1</div>

bro headless> dump #message
<p id="message">You clicked once!</p>

bro headless> eval document.getElementById("counter").textContent
Count: 1
```

### Piped

```
echo -e "dump #counter\nclick #btn\ndump #counter\nquit" | bro-headless apps/hello
```

Output:
```
<div id="counter"></div>
[console.log] Button clicked, count: 1
[changed]
<div id="counter">Count: 1</div>
```

### Script file

`test.txt`:
```
# Initial state
dump #counter

# Click the button 3 times
click #btn
click #btn
click #btn

# Check result
dump #counter
dump #message
diff
quit
```

```
bro-headless apps/hello test.txt
```

### Diff

`diff` compares the current DOM to the state at the last `dump` call:

```
bro headless> dump
bro headless> click #btn
bro headless> click #btn
bro headless> diff
- ...<p id="message">Click the button to get started.</p>...<div id="counter"></div>...
+ ...<p id="message">You clicked 2 times!</p>...<div id="counter">Count: 2</div>...
```

## How it works

- Uses a `NullRenderer` that does no drawing but provides text measurement (approximate, character-width based) so htmlayout can compute layout
- JS runs via QuickJS exactly as in the windowed engine
- Event dispatch mirrors the windowed engine: finds the element by selector, dispatches the event with bubbling, calls JS listeners
- DOM mutations from JS (e.g. `element.textContent = "..."`) are reflected immediately in subsequent `dump` output

## Notes

- `[INFO]` and `[console.log]` lines go to stderr; command output goes to stdout. You can separate them: `bro-headless apps/hello 2>/dev/null`
- Text measurement is approximate without real fonts. Layout positions may differ slightly from the windowed renderer.
- Virtual time starts at 0. Use `wait` to fire setTimeout/setInterval callbacks.
