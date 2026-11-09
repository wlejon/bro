#!/usr/bin/env python3
"""reduce-js.py — shrink a JS file while a bro-headless run of it still shows a symptom.

  scripts/reduce-js.py <file.js> <needle> [--appdir DIR] [--env K=V ...]
                       [--timeout S] [--absent] [-- extra bro-headless args]

Runs `bro-headless <appdir> <candidate.js>` (appdir defaults to the file's
directory; it needs an index.html) and keeps a candidate when its output
contains <needle> (with --absent: when it does not, e.g. reducing a missing
"PASSED" line). Removes chunks of lines, halving the chunk size when nothing
can go (delta debugging over lines), until no single line can be removed.
Writes <file>.min.js and prints it.

For the divergences scripts/exec-matrix.sh finds: copy the functions the
failing test reaches into one file with a call that shows the symptom, then
run it in the mode that fails, e.g. tier 2:
  scripts/reduce-js.py repro.js "operand has no virtual register" --env BRO_JIT_TIER=2
A candidate that no longer parses simply loses the symptom, so the result
stays a program; tidy it by hand (rename, give free names parameters) and
check the tidy version still shows it before pinning it as a test.

The binary is $BRO_HEADLESS, else ../build-release/bro-headless beside this
script's repo. The code cache is off for every run (BRO_CODE_CACHE=0), so a
cached compile never answers for an edited candidate.
"""
import os
import subprocess
import sys


def main():
    argv = sys.argv[1:]
    extra = []
    if '--' in argv:
        i = argv.index('--')
        argv, extra = argv[:i], argv[i + 1:]
    if len(argv) < 2 or argv[0] in ('-h', '--help'):
        print(__doc__)
        return 2
    src, needle = argv[0], argv[1]
    appdir = os.path.dirname(os.path.abspath(src))
    env = dict(os.environ, BRO_CODE_CACHE='0')
    timeout = 60.0
    absent = False
    rest = argv[2:]
    i = 0
    while i < len(rest):
        a = rest[i]
        if a == '--appdir':
            appdir = rest[i + 1]; i += 2
        elif a == '--env':
            k, _, v = rest[i + 1].partition('='); env[k] = v; i += 2
        elif a == '--timeout':
            timeout = float(rest[i + 1]); i += 2
        elif a == '--absent':
            absent = True; i += 1
        else:
            print(f'reduce-js: unknown option {a}', file=sys.stderr)
            return 2
    here = os.path.dirname(os.path.abspath(__file__))
    headless = os.environ.get('BRO_HEADLESS') or os.path.join(here, '..', 'build-release', 'bro-headless')
    if not os.path.exists(headless):
        print(f'reduce-js: no bro-headless at {headless} (set BRO_HEADLESS)', file=sys.stderr)
        return 2
    if not os.path.exists(os.path.join(appdir, 'index.html')):
        print(f'reduce-js: {appdir} has no index.html (pass --appdir)', file=sys.stderr)
        return 2
    trial = os.path.join(os.path.dirname(os.path.abspath(src)), '_reduce_trial.js')
    runs = 0

    def shows(lines):
        nonlocal runs
        runs += 1
        with open(trial, 'w') as f:
            f.write('\n'.join(lines) + '\n')
        try:
            r = subprocess.run([headless, *extra, appdir, trial], capture_output=True, text=True,
                               timeout=timeout, env=env)
            out = r.stdout + r.stderr
        except subprocess.TimeoutExpired:
            return False
        return (needle not in out) if absent else (needle in out)

    lines = open(src).read().split('\n')
    while lines and not lines[-1].strip():
        lines.pop()
    if not shows(lines):
        print('reduce-js: the original file does not show the symptom', file=sys.stderr)
        return 1
    n = 2
    while len(lines) >= 2:
        chunk = max(1, len(lines) // n)
        for start in range(0, len(lines), chunk):
            cand = lines[:start] + lines[start + chunk:]
            if cand and shows(cand):
                lines = cand
                n = max(n - 1, 2)
                break
        else:
            if chunk == 1:
                break
            n = min(len(lines), n * 2)
            continue
        print(f'  {len(lines)} lines ({runs} runs)', file=sys.stderr)
    try:
        os.remove(trial)
    except OSError:
        pass
    out = os.path.splitext(src)[0] + '.min.js'
    with open(out, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'{len(lines)} lines after {runs} runs -> {out}')
    print('\n'.join(lines))
    return 0


if __name__ == '__main__':
    sys.exit(main())
