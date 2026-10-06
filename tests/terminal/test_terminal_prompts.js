// <terminal>: a real shell marks its prompts, commands and output with
// OSC 133 (bash's PS1 / PS0 / PROMPT_COMMAND, the way shell-integration
// scripts do), and the element exposes them: t.commands with exit codes,
// selectOutput() and scrollToPrompt().
//
// bash is /bin/bash on POSIX and Git for Windows' bash on Windows (skipped
// where there is none).

const fs = require('fs');

const WIN = process.platform === 'win32';

function waitFor(pred, what, ms) {
    const end = Date.now() + (ms || 20000);
    while (Date.now() < end) {
        advanceTime(16);
        if (pred()) return true;
        wallSleep(5);
    }
    assert(pred(), 'timed out waiting for ' + what);
    return false;
}

function findBash() {
    const list = WIN
        ? ['C:/Program Files/Git/bin/bash.exe', 'C:/Program Files/Git/usr/bin/bash.exe']
        : ['/opt/homebrew/bin/bash', '/usr/local/bin/bash', '/usr/bin/bash', '/bin/bash'];
    for (const p of list) if (fs.existsSync(p)) return p;
    return null;
}

const BASH = findBash();
if (!bro.terminal || !bro.terminal.available) {
    skipTest('<terminal> is compiled out of this build (BRO_WITH_TERMINAL)');
} else if (!BASH) {
    skipTest('no bash to run');
} else {
    const t = document.createElement('terminal');
    t.setAttribute('cols', '60');
    t.setAttribute('rows', '6');
    document.body.appendChild(t);
    flush();
    const marks = [];
    t.addEventListener('promptmark', (e) => marks.push(e.detail));
    t.spawn({
        command: BASH,
        args: ['--norc', '--noprofile', '-i'],
        env: {
            TERM: 'xterm-256color',
            PS1: '\\[\\e]133;A\\a\\]$ \\[\\e]133;B\\a\\]',
            PS0: '\\[\\e]133;C\\a\\]',
            PROMPT_COMMAND: 'printf "\\033]133;D;%s\\007" $?',
        },
    });
    assert(t.running, 'bash runs: ' + BASH);
    waitFor(() => t.commands.length >= 1, 'the first prompt');

    const finished = () => t.commands.filter((c) => c.finished);
    const run = (line, count) => {
        t.write(line + '\r');
        waitFor(() => finished().length >= count && t.commands.length > count,
                'command ' + count + ' (' + line + ') and the next prompt: ' + JSON.stringify(t.commands));
    };
    run('echo out-one', 1);
    run('false', 2);
    run("printf 'l1\\nl2\\n'; (exit 7)", 3);
    for (let i = 4; i <= 9; ++i) run('echo filler-' + i, i);  // pushes the first prompts into history

    const done = finished();
    assert(done[0].exitCode === 0 && done[1].exitCode === 1 && done[2].exitCode === 7,
           'exit codes 0, 1, 7: ' + JSON.stringify(done.slice(0, 3).map((c) => c.exitCode)));
    assert(done[0].commandLine.includes('echo out-one'), 'the command line: ' + JSON.stringify(done[0].commandLine));
    assert(done[0].prompt && done[0].output && done[0].end, 'prompt, output and end positions: ' + JSON.stringify(done[0]));
    assert(done[0].output.row > done[0].prompt.row || done[0].output.col > done[0].prompt.col,
           'output follows its prompt');
    const last = t.commands[t.commands.length - 1];
    assert(!last.finished && last.exitCode === null, 'the current prompt is unfinished: ' + JSON.stringify(last));
    assert(marks.some((m) => m.mark === 'D' && m.exitCode === 7), 'a promptmark event carried exit 7: ' +
           JSON.stringify(marks.filter((m) => m.mark === 'D')));

    // Selecting a command's output.
    assert(t.selectOutput(done[2].output.row, 0), 'selectOutput at the printf command');
    const sel = () => t.selectionText().replace(/\r/g, '').trim();
    assert(sel() === 'l1\nl2', 'its output: ' + JSON.stringify(sel()));
    assert(t.selectOutput(done[0].output.row, 0), 'selectOutput at the first command (in history)');
    assert(sel() === 'out-one', 'its output: ' + JSON.stringify(sel()));
    assert(t.selectOutput(), 'selectOutput() with no cell: the last command');
    assert(sel() === 'filler-9', 'the last command\'s output: ' + JSON.stringify(sel()));
    t.clearSelection();

    // Jumping between prompts.
    assert(t.viewport.atBottom && t.viewport.historyRows > 0, 'history to jump through: ' + JSON.stringify(t.viewport));
    const promptRows = t.commands.map((c) => c.prompt.row);
    const shownAbove = () => t.viewport.topRow;
    t.scrollToPrompt(-1);
    const first = shownAbove();
    assert(!t.viewport.atBottom, 'scrolled up to a prompt: ' + JSON.stringify(t.viewport));
    assert(promptRows.includes(first), 'the view starts at a prompt row: ' + first + ' in ' + JSON.stringify(promptRows));
    t.scrollToPrompt(-1);
    assert(shownAbove() < first && promptRows.includes(shownAbove()), 'and the one before it: ' + shownAbove());
    t.scrollToPrompt(1);
    assert(shownAbove() === first, 'and forward again: ' + shownAbove());
    t.scrollToBottom();

    t.write('exit\r');
    waitFor(() => !t.running, 'bash exits');
}
