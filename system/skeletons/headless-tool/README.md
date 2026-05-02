# Headless tool

A starter for command-line tools and scripts driven by `bro-headless`.

## Run

```
bro-headless . run.js
```

This renders the page, writes `out.png` next to `run.js`, and exits. Inline JS works too:

```
bro-headless . -e "console.log(document.title)"
```

## What's here

- `run.js` — entry point. Edit this. Standard DOM + brokit APIs are available, plus the headless globals: `screenshot(path)`, `advanceTime(ms)`, `flush()`, `sleep(ms)`, `assert(cond, msg?)`.
- `index.html` — minimal page. The DOM is built in headless mode the same as windowed.
- `bro.json` — project manifest.

See `docs/headless.md` in the bro repo for the full headless reference.
