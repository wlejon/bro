// A shrink-to-fit flex-wrap row sized to its own max-content keeps its items
// on one line: the fit test allows the last-bit float difference between the
// sum that sized the box and the sum that breaks the line.

const root = document.getElementById('root');
const css =
    '#hud{position:absolute;top:16px;left:16px;display:flex;flex-wrap:wrap;gap:14px 20px;' +
    'padding:12px 16px;min-width:108px;font-family:Arial}' +
    '.lbl{font-size:11px;letter-spacing:0.08em;text-transform:uppercase}' +
    '.val{font-size:22px;font-weight:bold;line-height:1.2;margin-top:2px}';
function hud(a, b) {
    root.innerHTML = '<style>' + css + '</style><div id="hud">' +
        '<div class="stat" id="s1"><div class="lbl">Score</div><div class="val">' + a + '</div></div>' +
        '<div class="stat" id="s2"><div class="lbl">Best</div><div class="val">' + b + '</div></div></div>';
    flush();
    return [document.getElementById('s1').getBoundingClientRect(),
            document.getElementById('s2').getBoundingClientRect()];
}
for (const [a, b] of [['0', '2048'], ['128', '0'], ['4096', '65536'], ['7', '77']]) {
    const [s1, s2] = hud(a, b);
    assert(Math.abs(s1.top - s2.top) < 0.5,
           'stats ' + a + '/' + b + ' share a row: tops ' + s1.top + ' / ' + s2.top);
}

root.innerHTML = '';
