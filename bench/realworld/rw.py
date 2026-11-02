#!/usr/bin/env python3
"""Real-world three.js / pixi.js scenes: correctness and frame timing (README.md).

Scenes are bench/realworld/<scene>.html, each one page that runs unchanged in
node (state mode only), bro and Chromium; the load N and the mode come from
the URL query (Chromium, file:// with --allow-file-access-from-files: it
cannot reach a localhost server here), a generated config.js (bro: its
location.search is always empty) or argv key=value (node). See lib/harness.js.
Chromium returns files as RWBLOB console lines; bro writes them with fs into
its run dir (bro.json declares `files`, granted with --grant declared) and
logs to <run dir>/bro.log.

  python3 bench/realworld/rw.py l1 [--scenes a,b] [--tiers jit,aot,...] [--sizes min,max]
      Level 1: JS state (matrixWorld, culled + sorted render list, bone
      matrices, pixi transforms / batched vertices) after K frames with no
      renderer, node (reference) against bro-headless at each bronze tier.
  python3 bench/realworld/rw.py surface
      Level 2a: WebGL2 surface (extensions, getParameter, precision, context
      attributes) in windowed bro and Chromium, diffed.
  python3 bench/realworld/rw.py record [--scenes ...] [--n N]
      Level 2b: every WebGL call of frames 0-2 at a small N in both, diffed.
  python3 bench/realworld/rw.py shot --scene S [--n N] [--frame F]
      Level 3: readPixels of frame F in both, compared (compare_shots.py).
  python3 bench/realworld/rw.py render --browser bro|chromium --scene S [--n N] [--frames K]
      One windowed run, its state lines printed.
  python3 bench/realworld/rw.py time | sweep | perf ...
      Frame timing (timing.py): one timed run, the N sweep against Chromium's
      60 fps knee, and a perf profile of bro grouped by layer.

Work files go to $RW_WORK (default bench/realworld/.work). Windowed runs need
WAYLAND_DISPLAY / XDG_RUNTIME_DIR (defaulted to helm's wayland-0).
"""

import argparse
import base64
import difflib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BRO_ROOT = ROOT.parent.parent
BUILD = BRO_ROOT / "build-release"
BRO = BUILD / "bro"
BRO_HEADLESS = BUILD / "bro-headless"
BRONZE = BUILD / "bronze"
RT_LIB = BUILD / "shared" / "libbronze_runtime_shared.so"
WORK = Path(os.environ.get("RW_WORK", ROOT / ".work"))

AOT_BUILD_S = {}  # scene -> seconds `bronze build` took this invocation

SCENES = ["three_meshes", "three_instanced", "three_skinned", "three_lights",
          "pixi_sprites", "pixi_graphics", "pixi_text"]

# Planned sweep range per scene (min, max) and the small N for Levels 2-3.
SIZES = {
    "three_meshes": (100, 10000),
    "three_instanced": (1000, 200000),
    "three_skinned": (4, 300),
    "three_lights": (1, 64),
    "pixi_sprites": (1000, 200000),
    "pixi_graphics": (100, 10000),
    "pixi_text": (20, 2000),
}
SMALL = {"three_meshes": 50, "three_instanced": 500, "three_skinned": 4, "three_lights": 4,
         "pixi_sprites": 200, "pixi_graphics": 40, "pixi_text": 12}

# Level 1 tiers: name -> (kind, env). "jit" is the default tiered JIT of a folder app.
TIERS = {
    "jit": ("jit", {}),
    "tier0": ("jit", {"BRO_JIT_TIER": "0"}),
    "tier2": ("jit", {"BRO_JIT_TIER": "2"}),
    "aot": ("aot", {}),
    "deopt3": ("jit", {"BRASS_DEOPT_STRESS": "3"}),
    "gcstress": ("jit", {"BRONZE_GC_STRESS": "1"}),
}

WAYLAND_ENV = {"WAYLAND_DISPLAY": os.environ.get("WAYLAND_DISPLAY", "wayland-0"),
               "XDG_RUNTIME_DIR": os.environ.get("XDG_RUNTIME_DIR", "/run/user/1000")}


# ---------------------------------------------------------------- bro run dirs

def make_bro_app(name, page, cfg, aot=False):
    """A bro folder app at WORK/<name>: lib/scenes/vendor symlinked, the page
    copied to index.html, cfg written to config.js, output in <dir>/out.
    bro.json declares `files` (the run writes its output into <dir>/out, which
    an app without it may not: docs/apps.md, Permissions) and every launch
    grants what it declares (--grant declared); frames are uncapped (vsync
    off, maxFps 0: docs/settings.md)."""
    d = WORK / name
    if d.exists():
        shutil.rmtree(d)
    (d / "out").mkdir(parents=True)
    for sub in ("lib", "scenes", "vendor"):
        os.symlink(ROOT / sub, d / sub)
    os.symlink(ROOT / "package.json", d / "package.json")
    cfg = dict(cfg, out=str(d / "out"))
    (d / "config.js").write_text("globalThis.BENCH_CONFIG = " + json.dumps(cfg) + ";\n")
    html = (ROOT / page).read_text()
    start = html.index('<script type="module" src="')
    end = html.index("</script>", start) + len("</script>")
    entry = html[start:end].split('src="', 1)[1].split('"', 1)[0]
    if aot:
        html = html[:start] + html[end:]
    (d / "index.html").write_text(html)
    (d / "bro.json").write_text(json.dumps({"name": name, "width": cfg.get("w", 1280), "height": cfg.get("h", 720),
                                            "splash": False, "watch": False, "vsync": False, "maxFps": 0,
                                            "permissions": ["files"]}))
    if aot:
        cached = WORK / "aot" / Path(page).stem / "app.so"
        if not cached.exists():
            t0 = time.time()
            build_aot(d, d / entry)
            AOT_BUILD_S[Path(page).stem] = round(time.time() - t0, 1)
            cached.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(d / "app.so", cached)
        else:
            shutil.copy2(cached, d / "app.so")
    return d


def build_aot(d, entry):
    """app.so beside index.html: the host globals and native manifest printed
    by bro-headless, then `bronze build --emit-shared` against them. The module graph is
    compiled, config.js is not (a classic script, compiled at boot), so one
    app.so per scene serves every N; rw.py caches it under WORK/aot."""
    natives = d / "app.natives.json"
    res = subprocess.run([str(BRO_HEADLESS), str(d), "--print-host-globals", "--print-native-manifest", str(natives)],
                         capture_output=True, text=True, check=True)
    (d / "app.host-globals").write_text(res.stdout)
    env = dict(os.environ, BRONZE_SHARED_RT_LIB=str(RT_LIB), BRO_PROJECT_ROOT=str(BRO_ROOT))
    res = subprocess.run([str(BRONZE), "build", str(entry), "-o", str(d / "app.so"), "--emit-shared",
                          "--host-globals", str(d / "app.host-globals"), "--native-manifest", str(natives)],
                         env=env, capture_output=True, text=True)
    if res.returncode != 0:
        raise RuntimeError("bronze build failed:\n" + res.stdout[-2000:] + res.stderr[-2000:])


def rw_lines(text):
    """The harness's `RW <tag> <json>` lines out of a process's output."""
    out = []
    for line in text.splitlines():
        i = line.find("RW ")
        if i < 0:
            continue
        body = line[i + 3:]
        tag, _, js = body.partition(" ")
        if js.endswith('", source:') or '", source: ' in js:
            js = js.split('", source: ')[0]
        try:
            out.append((tag, json.loads(js)))
        except json.JSONDecodeError:
            out.append((tag, js))
    return out


# ---------------------------------------------------------------- Level 1

def run_node_state(scene, n, frames, cp):
    res = subprocess.run(["node", str(ROOT / "scenes" / f"{scene}.js"), "mode=state", f"n={n}", f"frames={frames}", f"cp={cp}"],
                         capture_output=True, text=True)
    return res.returncode, rw_lines(res.stdout), res.stderr


def run_headless_state(scene, n, frames, cp, tier):
    kind, env_extra = TIERS[tier]
    cfg = {"mode": "state", "n": n, "frames": frames, "cp": cp, "quit": 0}
    d = make_bro_app(f"l1_{scene}_{n}_{tier}", f"{scene}.html", cfg, aot=(kind == "aot"))
    env = dict(os.environ, BRO_TEST_VK_VALIDATION="0", **env_extra)
    res = subprocess.run([str(BRO_HEADLESS), "--grant", "declared", str(d), "--no-gpu", "-e", "flush()"], env=env, capture_output=True, text=True)
    lines = rw_lines(res.stdout)
    errs = [l for l in (res.stdout + res.stderr).splitlines() if "rror" in l and "RW " not in l][:8]
    return res.returncode, lines, "\n".join(errs)


def compare_states(ref, got):
    """'match', 'ulp' (exact hash differs, f32 hash and counts agree) or a mismatch description."""
    rs = [o for t, o in ref if t == "state"]
    gs = [o for t, o in got if t == "state"]
    if not gs:
        errs = [o for t, o in got if t == "error"]
        return "no-output" + (": " + errs[0]["msg"] if errs else "")
    if len(rs) != len(gs):
        return f"checkpoints {len(rs)} vs {len(gs)}"
    worst = "match"
    ulps = []
    for r, g in zip(rs, gs):
        for k in r:
            a, b = r[k], g.get(k)
            if a == b:
                continue
            if k == "sample":
                if all(abs(x - y) <= 1e-12 * max(1.0, abs(x)) for x, y in zip(a, b)):
                    continue
                return f"MISMATCH f={r['f']} sample: {a} vs {b}"
            if isinstance(a, str) and isinstance(b, str) and a.count(":") == 2 and b.count(":") == 2:
                ea, fa, ca = a.split(":")
                eb, fb, cb = b.split(":")
                if fa == fb and ca == cb:
                    worst = "ulp"
                    ulps.append(f"{k}@{r['f']}")
                    continue
            return f"MISMATCH f={r['f']} {k}: {a} vs {b}"
    return worst + (" (" + ",".join(ulps[:6]) + ")" if ulps else "")


def cmd_l1(args):
    scenes = args.scenes.split(",") if args.scenes else SCENES
    tiers = args.tiers.split(",")
    rows = []
    for scene in scenes:
        lo, hi = SIZES[scene]
        sizes = {"min": lo, "max": hi, "small": SMALL[scene]}
        for sz in args.sizes.split(","):
            n = sizes[sz] if sz in sizes else int(sz)
            frames = args.frames
            cp = f"0,1,2,{frames // 2},{frames - 1}"
            rc, ref, err = run_node_state(scene, n, frames, cp)
            if rc != 0 or not ref:
                rows.append((scene, n, "node", f"node failed rc={rc} {err[-300:]}"))
                continue
            for tier in tiers:
                # The slow tiers run at the smallest N only (gcstress collects at
                # every allocation: the library import alone takes minutes).
                if tier in ("gcstress", "tier0") and n > lo:
                    continue
                f = min(frames, 10) if tier == "gcstress" else frames
                if f != frames:
                    cp2 = f"0,1,2,{f // 2},{f - 1}"
                    _, ref2, _ = run_node_state(scene, n, f, cp2)
                else:
                    cp2, ref2 = cp, ref
                try:
                    rc, got, err = run_headless_state(scene, n, f, cp2, tier)
                    verdict = compare_states(ref2, got)
                    if not verdict.startswith(("match", "ulp")) and err:
                        verdict += " | " + err.replace("\n", " / ")[:400]
                except Exception as e:  # noqa: BLE001
                    verdict = f"harness error: {e}"[:600]
                rows.append((scene, n, tier, verdict))
                print(f"{scene:16} N={n:<7} {tier:9} {verdict}", flush=True)
    if args.json_out:
        Path(args.json_out).write_text(json.dumps(rows, indent=1))


# ---------------------------------------------------------------- windowed runs

class Server:
    """Placeholder kept for the call sites: Chromium cannot reach a localhost
    server from this machine's sandbox, so pages load from file:// and send
    their blobs back as RWBLOB console lines (lib/harness.js emitBlob)."""

    def close(self):
        pass


FATAL = ("Uncaught", "SyntaxError", "Failed to load module", "Failed to fetch dynamically")


def read_until_done(proc, log):
    """Read a browser's output until the page's RW_DONE marker (or a fatal
    script error), keeping every line; returns the status word."""
    status = "eof"
    live = open(WORK / "live.log", "w")
    for line in proc.stdout:
        log.append(line.rstrip("\n"))
        live.write(line)
        live.flush()
        if "RW_DONE" in line:
            status = line.split("RW_DONE", 1)[1].strip().split()[0].strip('",')
            break
        if any(p in line for p in FATAL) and "RW " not in line and "RWBLOB" not in line:
            status = "fatal"
            break
    live.close()
    return status


def collect_blobs(log, out):
    """Reassemble `RWBLOB <name> <i> <count> <base64>` console lines into files."""
    parts = {}
    for line in log:
        i = line.find('RWBLOB ')
        if i < 0:
            continue
        body = line[i + 7:].split('", source:')[0].strip().strip('"')
        name, idx, count, b64 = body.split(" ", 3)
        parts.setdefault(name, [None] * int(count))[int(idx)] = base64.b64decode(b64)
    for name, chunks in parts.items():
        if any(c is None for c in chunks):
            print("blob", name, "incomplete")
            continue
        (out / os.path.basename(name)).write_bytes(b"".join(chunks))


def run_chromium(server, page, cfg, out):
    out.mkdir(parents=True, exist_ok=True)
    q = "&".join(f"{k}={v}" for k, v in cfg.items())
    url = (ROOT / page).as_uri() + "?" + q
    prof = tempfile.mkdtemp(prefix="chprof_", dir=str(WORK))
    w, h = cfg.get("w", 1280), cfg.get("h", 720)
    env = dict(os.environ, **WAYLAND_ENV)
    cmd = ["chromium", "--ozone-platform=wayland", f"--user-data-dir={prof}", "--no-first-run",
           "--no-default-browser-check", "--enable-logging=stderr", "--v=0", "--ignore-gpu-blocklist",
           "--allow-file-access-from-files", "--disable-extensions",
           "--disable-background-timer-throttling", "--disable-renderer-backgrounding",
           # uncapped frames, as bro's vsync false / maxFps 0
           "--disable-gpu-vsync", "--disable-frame-rate-limit",
           f"--window-size={w},{h}", f"--app={url}"]
    proc = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    log = []
    try:
        status = read_until_done(proc, log)
    finally:
        proc.terminate()
        proc.wait()
        shutil.rmtree(prof, ignore_errors=True)
    collect_blobs(log, out)
    (out / "browser.log").write_text("\n".join(l for l in log if "RWBLOB" not in l) + "\n")
    return status, log


def run_bro(page, cfg, name, aot=False, env_extra=None, perf_data=None):
    """One windowed bro run of `page`; with perf_data, under `perf record`
    (user space, CLOCK_MONOTONIC timestamps, JIT code named through brass's
    perf map) so a window of it can be reported against the trace clock."""
    d = make_bro_app(name, page, cfg, aot=aot)
    env = dict(os.environ, BRO_TEST_VK_VALIDATION="0", **WAYLAND_ENV, **(env_extra or {}))
    cmd = [str(BRO), "--grant", "declared", str(d)]
    if perf_data:
        env["BRASS_PERF_MAP"] = "1"
        cmd = ["perf", "record", "-q", "-k", "CLOCK_MONOTONIC", "-e", "cycles:u", "-F", "1999",
               "-o", str(perf_data), "--"] + cmd
    # An app without an id logs (console included) to bro.log in the working
    # directory rather than stdout; the page ends the run with bro.quit()
    # (its watchdog too). bro moves its own stdout onto that log at startup,
    # so a pipe would see EOF at once: wait for the process instead.
    proc = subprocess.Popen(cmd, cwd=str(d), env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    log = []
    status = "eof"
    try:
        proc.wait()
    finally:
        if proc.poll() is None:
            proc.terminate()
            proc.wait()
    logf = d / "bro.log"
    if logf.exists():
        log += logf.read_text(errors="replace").splitlines()
        for line in log:
            if "RW_DONE" in line:
                status = line.split("RW_DONE", 1)[1].strip().split()[0]
    (d / "out" / "browser.log").write_text("\n".join(log) + "\n")
    return status, log, d / "out"


def windowed(browser, page, cfg, name, server=None):
    if browser == "bro":
        return run_bro(page, cfg, name)
    out = WORK / name / "out"
    if out.exists():
        shutil.rmtree(out)
    status, log = run_chromium(server, page, cfg, out)
    return status, log, out


def cmd_render(args):
    cfg = {"mode": "render", "n": args.n or SMALL[args.scene], "frames": args.frames}
    server = Server() if args.browser == "chromium" else None
    try:
        status, log, out = windowed(args.browser, f"{args.scene}.html", cfg, f"render_{args.browser}_{args.scene}", server)
    finally:
        if server:
            server.close()
    print("status", status)
    for line in log:
        if "RW " in line:
            print(line[line.find("RW "):][:400])


# ---------------------------------------------------------------- Level 2a

def cmd_surface(args):
    server = Server()
    res = {}
    try:
        for browser in ("bro", "chromium"):
            status, log, out = windowed(browser, "surface.html", {"mode": "surface", "w": 640, "h": 480}, f"surface_{browser}", server)
            p = out / "surface.json"
            if not p.exists():
                print(browser, "no surface.json; status", status)
                print("\n".join(l for l in log if "RW" in l or "rror" in l)[-2000:])
                return
            res[browser] = json.loads(p.read_text())
    finally:
        server.close()
    (WORK / "surface_diff.json").write_text(json.dumps(res, indent=1))
    b, c = res["bro"], res["chromium"]
    be, ce = set(b["extensions"]), set(c["extensions"])
    print("== extensions: bro %d, chromium %d" % (len(be), len(ce)))
    print("  only chromium:", ", ".join(sorted(ce - be)))
    print("  only bro:     ", ", ".join(sorted(be - ce)))
    for sect in ("parameters", "precision", "attributes", "formats", "misc"):
        print("== " + sect)
        keys = sorted(set(b.get(sect, {})) | set(c.get(sect, {})))
        for k in keys:
            vb, vc = b.get(sect, {}).get(k), c.get(sect, {}).get(k)
            mark = "  " if vb == vc else "!="
            if vb != vc or args.all:
                print(f"  {mark} {k:44} bro={json.dumps(vb)}  chromium={json.dumps(vc)}")


# ---------------------------------------------------------------- Level 2b

def parse_calls(path):
    calls, strings, cur = [], {}, None
    for line in path.read_text().splitlines():
        if line.startswith("#### str "):
            cur = line[9:].strip()
            strings[cur] = []
            continue
        if cur is not None:
            strings[cur].append(line)
        else:
            calls.append(line)
    return calls, {k: "\n".join(v) for k, v in strings.items()}


def cmd_record(args):
    scenes = args.scenes.split(",") if args.scenes else SCENES
    server = Server()
    try:
        for scene in scenes:
            n = args.n or SMALL[scene]
            cfg = {"mode": "record", "n": n, "rec": 3}
            got = {}
            for browser in ("bro", "chromium"):
                status, log, out = windowed(browser, f"{scene}.html", cfg, f"rec_{browser}_{scene}", server)
                p = out / "calls.txt"
                if not p.exists():
                    print(scene, browser, "no calls.txt; status", status)
                    print("\n".join(l for l in log if "RW" in l or "rror" in l)[-1500:])
                    break
                got[browser] = parse_calls(p)
            if len(got) != 2:
                continue
            (bc, bs), (cc, cs) = got["bro"], got["chromium"]
            diff = list(difflib.unified_diff(bc, cc, "bro", "chromium", n=1, lineterm=""))
            (WORK / f"rec_{scene}.diff").write_text("\n".join(diff) + "\n")
            nd = sum(1 for l in diff if l[:1] in "+-" and not l.startswith(("+++", "---")))
            print(f"== {scene} N={n}: bro {len(bc)} calls, chromium {len(cc)} calls, {nd} differing lines -> {WORK / f'rec_{scene}.diff'}")
            # per-function call counts, the quickest read of what differs
            from collections import Counter
            cnt = lambda calls: Counter(l.split("\t", 1)[1].split("(", 1)[0] for l in calls if "\t" in l)
            # calls per frame (-1 is setup, before the first rAF)
            per = lambda calls: Counter(l.split("\t", 1)[0] for l in calls if "\t" in l)
            pb, pc = per(bc), per(cc)
            print("   per frame (bro/chromium): " + "  ".join(
                f"f{f}: {pb[f]}/{pc[f]}" + ("" if pb[f] == pc[f] else " !=") for f in sorted(set(pb) | set(pc), key=int)))
            for f in sorted(set(pb) | set(pc), key=int):
                fb = cnt([l for l in bc if l.startswith(f + "\t")])
                fc = cnt([l for l in cc if l.startswith(f + "\t")])
                for fn in sorted(set(fb) | set(fc)):
                    if fb[fn] != fc[fn]:
                        print(f"     f{f} {fn:38} bro {fb[fn]:5}  chromium {fc[fn]:5}")
            kb, kc = cnt(bc), cnt(cc)
            for fn in sorted(set(kb) | set(kc)):
                if kb[fn] != kc[fn]:
                    print(f"   {fn:40} bro {kb[fn]:5}  chromium {kc[fn]:5}")
            for h in sorted(set(bs) | set(cs)):
                if (h in bs) != (h in cs):
                    print("   string only in", "bro" if h in bs else "chromium", h)
    finally:
        server.close()


# ---------------------------------------------------------------- Level 3

def cmd_shot(args):
    scene = args.scene
    n = args.n or SMALL[scene]
    cfg = {"mode": "shot", "n": n, "shot": args.frame}
    server = Server()
    outs = {}
    try:
        for browser in ("bro", "chromium"):
            status, log, out = windowed(browser, f"{scene}.html", cfg, f"shot_{browser}_{scene}", server)
            if not (out / "shot.rgba").exists():
                print(browser, "no shot; status", status)
                print("\n".join(l for l in log if "RW" in l or "rror" in l)[-1500:])
                return
            outs[browser] = out / "shot.rgba"
    finally:
        server.close()
    cmp = [sys.executable, str(ROOT / "compare_shots.py"), str(outs["bro"]), str(outs["chromium"]),
           "--size", "1280x720", "--tol", str(args.tol), "--out", str(WORK / f"shot_{scene}")]
    subprocess.run(cmp, check=False)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("l1")
    p.add_argument("--scenes", default="")
    p.add_argument("--tiers", default="jit,aot,tier0,deopt3,gcstress")
    p.add_argument("--sizes", default="min,max")
    p.add_argument("--frames", type=int, default=120)
    p.add_argument("--json-out", default="")
    p = sub.add_parser("surface")
    p.add_argument("--all", action="store_true")
    p = sub.add_parser("record")
    p.add_argument("--scenes", default="")
    p.add_argument("--n", type=int, default=0)
    p = sub.add_parser("shot")
    p.add_argument("--scene", required=True)
    p.add_argument("--n", type=int, default=0)
    p.add_argument("--frame", type=int, default=30)
    p.add_argument("--tol", type=int, default=8)
    p = sub.add_parser("render")
    p.add_argument("--browser", required=True, choices=["bro", "chromium"])
    p.add_argument("--scene", required=True)
    p.add_argument("--n", type=int, default=0)
    p.add_argument("--frames", type=int, default=60)
    sys.path.insert(0, str(ROOT))
    import timing
    extra = timing.add_commands(sub)
    args = ap.parse_args()
    WORK.mkdir(parents=True, exist_ok=True)
    # app.so is built once per scene per invocation (sources change between).
    shutil.rmtree(WORK / "aot", ignore_errors=True)
    # Chromium's helpers can still be writing a profile when its main process
    # has exited, so a profile dir a run deleted may come back: sweep them.
    if args.cmd != "l1":
        for p in WORK.glob("chprof_*"):
            shutil.rmtree(p, ignore_errors=True)
    cmds = {"l1": cmd_l1, "surface": cmd_surface, "record": cmd_record, "shot": cmd_shot, "render": cmd_render}
    cmds.update(extra)
    cmds[args.cmd](args)


if __name__ == "__main__":
    main()
