// Test bro.eye.audit on live DOM + pixel-grounded defects, then verify fix drops issues to 0.

document.body.innerHTML = `
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; font-family: Arial, sans-serif; }
  html, body { background: #181a20; width: 100%; height: 100%; position: relative; }
  #bg-canvas { position: absolute; left: 20px; top: 20px; width: 280px; height: 60px; pointer-events: none; }
  #bad-text {
    position: absolute; left: 28px; top: 34px; width: 240px; height: 28px;
    font-size: 16px; color: #dcdcdc; background: transparent;
  }
  #clipped-box {
    position: absolute; left: 20px; top: 100px; width: 70px; height: 22px;
    overflow: hidden; background: #222630; color: #f5f5f5; font-size: 14px; white-space: nowrap;
  }
  #ov-a { position: absolute; left: 20px; top: 145px; width: 140px; height: 28px; background: #202530; color: #f0f0f0; font-size: 14px; }
  #ov-b { position: absolute; left: 90px; top: 155px; width: 140px; height: 28px; background: #302025; color: #f0f0f0; font-size: 14px; }
  #tiny-btn {
    position: absolute; left: 20px; top: 210px; width: 10px; height: 10px;
    background: #3b82f6; border: none;
  }
  #offscreen-btn {
    position: absolute; left: 2000px; top: 20px; width: 90px; height: 32px;
    background: #10b981; color: #05100c; font-size: 14px;
  }
</style>
<canvas id="bg-canvas" width="280" height="60"></canvas>
<div id="bad-text">Unreadable on bright canvas</div>
<div id="clipped-box">This long label definitely overflows horizontally</div>
<div id="ov-a">First label</div>
<div id="ov-b">Colliding label</div>
<button id="tiny-btn" aria-label="tiny"></button>
<button id="offscreen-btn">Far away</button>
`;

// Paint a bright light background directly onto the canvas under #bad-text.
// A DOM-only inspect() sees `background: transparent` and cannot know the pixels are #eaeaea!
const cv = document.getElementById('bg-canvas');
const ctx = cv.getContext('2d');
ctx.fillStyle = '#eaeaea';
ctx.fillRect(0, 0, 280, 60);
flush();

const badAudit = bro.eye.audit();
assert(badAudit.ok === false, 'buggy UI fails audit');
assert(badAudit.errorCount >= 2, `expected >= 2 errors, got ${badAudit.errorCount}`);

const kinds = new Set(badAudit.issues.map(i => i.kind));
assert(kinds.has('text-contrast'), 'caught pixel-level low text contrast over canvas');
assert(kinds.has('overflow-clip'), 'caught non-scrollable overflow clipping');
assert(kinds.has('overlap'), 'caught overlapping sibling text elements');
assert(kinds.has('offscreen'), 'caught offscreen element');
assert(kinds.has('small-hit-target'), 'caught undersized button hit target');

const contrastIssue = badAudit.issues.find(i => i.kind === 'text-contrast');
assert(contrastIssue.wcag < 2.0, `WCAG contrast should be very low, got ${contrastIssue.wcag}`);

const os = require('os');
const path = require('path');

// Generate annotated visual contact sheet of defects.
const auditPng = path.join(os.tmpdir(), 'eye_test_audit.png').replace(/\\/g, '/');
const sheet = bro.eye.visualize(null, { mode: 'audit', path: auditPng });
assert(sheet.width > 0 && sheet.height > 0 && sheet.path === auditPng,
    'audit visualization PNG generated');

// Now fix every defect and re-audit.
document.getElementById('bad-text').style.color = '#111111';
document.getElementById('clipped-box').style.width = '360px';
document.getElementById('ov-b').style.left = '20px';
document.getElementById('ov-b').style.top = '185px';
const tiny = document.getElementById('tiny-btn');
tiny.style.top = '230px';
tiny.style.width = '88px';
tiny.style.height = '32px';
document.getElementById('offscreen-btn').style.left = '20px';
document.getElementById('offscreen-btn').style.top = '280px';

// Verify oklch() color resolution in wide elements and WCAG disabled-control exemption.
const wideOklch = document.createElement('div');
wideOklch.id = 'wide-oklch';
wideOklch.textContent = 'Wide oklch heading';
wideOklch.style.cssText = 'position:absolute;left:20px;top:330px;width:960px;height:24px;font-size:18px;color:oklch(0.96 0.004 255);background:#181a20;';
document.body.appendChild(wideOklch);

const disBtn = document.createElement('button');
disBtn.id = 'disabled-btn';
disBtn.textContent = 'Disabled';
disBtn.setAttribute('disabled', '');
disBtn.style.cssText = 'position:absolute;left:20px;top:370px;width:88px;height:28px;opacity:0.4;color:#80858c;background:#181a20;';
document.body.appendChild(disBtn);

// Verify floating HUD button over full-stage canvas and col-resize splitter don't false-alarm.
const stageCanvas = document.createElement('canvas');
stageCanvas.style.cssText = 'position:absolute;left:420px;top:20px;width:200px;height:140px;background:#20242c;';
document.body.appendChild(stageCanvas);
const hudWrap = document.createElement('div');
hudWrap.style.cssText = 'position:absolute;left:460px;top:110px;z-index:5;';
const hudBtn = document.createElement('button');
hudBtn.textContent = 'Next';
hudBtn.style.cssText = 'width:64px;height:36px;color:#ffffff;background:#101214;';
hudWrap.appendChild(hudBtn);
document.body.appendChild(hudWrap);

const grip = document.createElement('div');
grip.style.cssText = 'position:absolute;left:400px;top:20px;width:12px;height:28px;cursor:col-resize;';
grip.addEventListener('click', (e) => e.stopPropagation());
document.body.appendChild(grip);
flush();

const cleanAudit = bro.eye.audit();
assert(cleanAudit.ok === true, `fixed UI should pass audit, remaining issues: ${JSON.stringify(cleanAudit.issues)}`);
assert(cleanAudit.errorCount === 0 && cleanAudit.warningCount === 0,
    `zero defects remain after fixes, got: ${JSON.stringify(cleanAudit.issues)}`);
assert(cleanAudit.score === 100, `clean audit score 100, got ${cleanAudit.score}`);
