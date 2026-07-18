# tests/manual — hand-run harnesses

Scripts here are NOT discovered by `tests/run_tests.sh` (it only picks up
`test_*.js`). They are kept for hand-driven investigation and depend on things
the automated suite must not: sibling-repo model weights and datasets
(`../brosoundml/weights/...`, `../brosoundml-data/...`), the source tree
layout, or a human judging the output.

Run any of them from the bro repo root:

```bash
./build/Release/bro-headless.exe tests/test_app tests/manual/<script>.js
```

| Script | Needs | What it does |
|---|---|---|
| `coverage.js` | bro + broworkshop source trees | Scans `src/js/*_bindings.cpp` vs test files and prints a JS-API coverage report (analysis tool, not pass/fail) |
| `net_roundtrip.js` | — | Original single-context bro.net host/connect/send smoke; superseded for CI purposes by `tests/net/test_net_loopback.js` and `test_net_channels_clone.js` |
| `repro_wake_passes.js` | wake weights + positives dataset | Multi-pass wake detection through the real mic tap (suspend/resume, no-warmup regression) |
| `smoke_voice_pipeline.js` | STT/LM/TTS weights | End-to-end bro.stt → bro.lm → bro.tts pass; writes a WAV to listen to |
| `smoke_wake_binding.js` | wake weights (self-skips without) | bro.wake JS surface wiring on silence |
| `smoke_wake_positive.js` | wake weights + positives dataset | Detector fires on known-positive clips |
| `smoke_wake_tone.js` | wake weights | Probes false-fires on tones/noise (model-quality diagnostic) |

## Hand-run apps

Directories here are whole bro apps rather than scripts — run them windowed:

```bash
./build/Release/bro.exe tests/manual/<app>
```

| App | What it demonstrates |
|---|---|
| `multiwindow_demo` | Multi-window end to end: the main window opens a tool-palette in its own OS window (`bro.window.open('palette')`, sized/titled by the palette's own `bro.json`), the palette posts the colour you click back through `bro.window.parent.postMessage`, the main window applies it and posts its current colour back, and the palette's Close button self-closes with `window.close()`. See [docs/window-api.js](../../docs/window-api.js). |
