#!/usr/bin/env python3
"""Make machine-level text safe to print: debugger output, disassembly, perf
reports, hex dumps.

An agent's tool output travels over the network, and a filter there rejects
requests that look like raw machine data (long hex addresses, byte columns,
non-printable bytes), which ends the session. Pipe such output through this
before printing it:

    perf report --stdio ... | scripts/safeview.py --max 60
    objdump -d f.o | scripts/safeview.py --map perf-1234.map --grep 'cmp|j[a-z]+ '
    scripts/safeview.py --stats trace.txt          # counts only

What it does to each line:
  - bytes outside printable ASCII become '?'
  - an address (0x… of five or more digits, or a bare word of eight or more
    hex digits) becomes a label: `name+off` when --map names the symbol it
    falls in (perf map format: `start size name`, hex), else `@N`, the same N
    for the same address through the run; `@N+d` when it lies within 4096
    bytes above an address already seen
  - a column of raw instruction bytes (`48 8b 05 …`) is removed
  - a short hex number (0x1f) is printed in decimal
  - a C++ name is cut at its first template or argument list
  - the line is cut at --width characters

The label table is written to --labels (default: none) so a later script can
turn a label back into the address without it being printed.
"""
import argparse
import bisect
import re
import sys

ADDR = re.compile(r"0x[0-9a-fA-F]{5,}|\b(?=[0-9a-fA-F]*[0-9])[0-9a-fA-F]{8,}\b")
FLOAT = re.compile(r"(\d+\.\d+(?:[eE][-+]?\d+)?)")
SMALL_HEX = re.compile(r"\b0x([0-9a-fA-F]{1,4})\b")
BYTE_COLUMN = re.compile(r"(?:\b[0-9a-fA-F]{2}\b[ \t]){3,}")
CXX_TAIL = re.compile(r"(<[^<>]*>|\([^()]*\))")


class Labels:
    def __init__(self, map_path):
        self.starts, self.syms = [], []
        if map_path:
            rows = []
            with open(map_path, errors="replace") as f:
                for line in f:
                    parts = line.split(None, 2)
                    if len(parts) == 3:
                        try:
                            rows.append((int(parts[0], 16), int(parts[1], 16), parts[2].strip()))
                        except ValueError:
                            pass
            rows.sort()
            self.starts = [r[0] for r in rows]
            self.syms = rows
        self.seen = {}      # address -> label
        self.order = []     # sorted addresses that own a bare label

    def of(self, addr):
        if addr in self.seen:
            return self.seen[addr]
        label = None
        i = bisect.bisect_right(self.starts, addr) - 1
        if i >= 0:
            start, size, name = self.syms[i]
            if addr < start + max(size, 1):
                label = f"{short_name(name)}+{addr - start}"
        if label is None:
            j = bisect.bisect_right(self.order, addr) - 1
            if j >= 0 and addr - self.order[j] < 4096:
                label = f"{self.seen[self.order[j]]}+{addr - self.order[j]}"
            else:
                label = f"@{len(self.order) + 1}"
                bisect.insort(self.order, addr)
        self.seen[addr] = label
        return label


def strip_cxx(text):
    prev = None
    while prev != text:
        prev = text
        text = CXX_TAIL.sub("", text)
    return text


def short_name(name):
    return strip_cxx(name).strip()[:80]


def clean(line, labels, width):
    line = "".join(c if 32 <= ord(c) < 127 or c == "\t" else "?" for c in line.rstrip("\n"))
    line = BYTE_COLUMN.sub("", line)
    # A decimal number's digits are not an address (6.95e-310 is a float).
    parts = FLOAT.split(line)
    for i in range(0, len(parts), 2):
        parts[i] = ADDR.sub(lambda m: labels.of(int(m.group(0), 16)), parts[i])
    line = "".join(parts)
    line = SMALL_HEX.sub(lambda m: str(int(m.group(1), 16)), line)
    if "::" in line and len(line) > width:
        line = strip_cxx(line)
    return line[:width]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", nargs="?", help="input (default: stdin)")
    ap.add_argument("--map", help="perf map of JIT symbols: `start size name` per line, hex")
    ap.add_argument("--labels", help="write the label table here")
    ap.add_argument("--grep", help="keep only lines matching this regex (after cleaning)")
    ap.add_argument("--max", type=int, default=80, help="lines to print (default 80)")
    ap.add_argument("--width", type=int, default=160)
    ap.add_argument("--stats", action="store_true", help="print counts only: lines, distinct addresses, top words")
    args = ap.parse_args()

    labels = Labels(args.map)
    keep = re.compile(args.grep) if args.grep else None
    src = open(args.file, "rb") if args.file else sys.stdin.buffer
    shown = total = 0
    words = {}
    for raw in src:
        line = clean(raw.decode("latin-1"), labels, args.width)
        if keep and not keep.search(line):
            continue
        total += 1
        if args.stats:
            for w in re.findall(r"[A-Za-z_][A-Za-z0-9_:.]{2,}", line):
                words[w] = words.get(w, 0) + 1
        elif shown < args.max:
            print(line)
            shown += 1
    if args.stats:
        print(f"lines {total}, distinct addresses {len(labels.seen)}")
        for w, n in sorted(words.items(), key=lambda kv: -kv[1])[:args.max]:
            print(f"{n:8d}  {w}")
    elif total > shown:
        print(f"... {total - shown} more lines")
    if args.labels:
        with open(args.labels, "w") as f:
            for addr, label in sorted(labels.seen.items()):
                f.write(f"{label} {addr:x}\n")


if __name__ == "__main__":
    main()
