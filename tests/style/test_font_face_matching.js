// @font-face: CSS font matching across a family declared once per weight, and
// srcs resolved against the stylesheet that declares them.
//
// A family declared four times (Inter Regular/Medium/SemiBold/Bold at
// 400/500/600/700) used to draw its first face for every weight, and every
// @font-face src was resolved against the document rather than the sheet, so
// `url("../fonts/x.ttf")` in ui/css/tokens.css never loaded. The fixture app
// (font_face_app/) needs its own linked, @imported sheets and its own /lib
// mount, so it runs in a child bro-headless with font_face_app/measure.js,
// which asserts the widths per weight, the in-between weights' matches and
// synthesized bold. The fonts are Inter (SIL OFL 1.1, licence beside them).

const cp = require('child_process');
const path = require('path');

const exeName = process.platform === 'win32' ? 'bro-headless.exe' : 'bro-headless';
const exe = path.join(process.env.BRO_EXE_DIR, exeName);
const app = path.join(process.env.BRO_APP_DIR, '..', 'style', 'font_face_app');
const measure = path.join(app, 'measure.js');

const r = cp.spawnSync(exe, [app, measure], { encoding: 'utf8' });
const out = (r.stdout || '') + (r.stderr || '');
assert(r.status === 0,
       'font_face_app child bro-headless exited ' + r.status +
       '\n--- child output ---\n' + out.split(/\r?\n/)
         .filter((l) => /ASSERT|MEASURE_JSON|font-face|ERROR|WARN/.test(l)).join('\n'));
assert(out.includes('font-face matching: ok'), 'the child ran to the end:\n' + out);
const line = out.split(/\r?\n/).find((l) => l.includes('MEASURE_JSON '));
assert(line, 'the child printed its widths');
console.log('widths per weight: ' + line.slice(line.indexOf('MEASURE_JSON ') + 13));
