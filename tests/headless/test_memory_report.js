// `bro-ctl mem` (docs/agent-control.md): what the process costs and who holds
// it, driven in-process like the other control commands.

function run() {
    var id = controlCommand.apply(null, arguments);
    for (var i = 0; i < 100; i++) {
        var r = controlResult(id);
        if (r) return r;
        advanceTime(16);
    }
    throw new Error('no reply to ' + Array.prototype.join.call(arguments, ' '));
}

var r = run('mem');
assert(r.ok, 'mem succeeds: ' + r.payload);
var text = r.payload;
['process', 'threads', 'js ', 'skia', 'images', 'subsystems'].forEach(function(k) {
    assert(text.indexOf(k) >= 0, 'mem reports ' + k + ':\n' + text);
});

// Optional subsystems start on first use: this page has used none of them.
['physics:off', 'audio:off', 'audio-inference:off', 'net:off', 'steam:off'].forEach(function(k) {
    if (text.indexOf(k.split(':')[0] + ':') >= 0)
        assert(text.indexOf(k) >= 0, 'an unused subsystem is not running (' + k + '):\n' + text);
});
// ...and the first use starts one.
var ctx = new AudioContext();
assert(run('mem').payload.indexOf('audio:on') >= 0, 'an AudioContext starts the audio engine');
ctx.close && ctx.close();

var detail = run('mem', '--detail', '--trim');
assert(detail.ok, 'mem --detail --trim succeeds: ' + detail.payload);
assert(detail.payload.indexOf('subsystems') >= 0, 'the detailed report is a report');
