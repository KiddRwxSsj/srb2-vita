#!/usr/bin/env python3
"""Summarize a Vita benchmark-result.txt and name thinker and state action functions.

usage: vita_bench_report.py RESULT.txt ELF
"""
import os
import re
import subprocess
import sys
from collections import defaultdict

BENCH = re.compile(
    r"^(Bench [^:]+): (\d+) frames in [\d.]+ s, ([\d.]+) fps, avg ([\d.]+) ms, 1% low ([\d.]+) ms, "
    r"max ([\d.]+) ms, hitches (\d+), game speed (\d+)%.*?\| tic: logic ([\d.]+) .*?\| view: ([\d.]+) ms")
FUNC = re.compile(r"^(Bench [^ ]+(?: [^ ]+)?) func (\w+) (0x[0-9a-f]+): (.*)$")
ACTION = re.compile(r"^(Bench [^ ]+(?: [^ ]+)?) action (0x[0-9a-f]+): (.*)$")


def function_starts(elf):
    nm = os.path.join(os.environ.get("VITASDK", "/usr/local/vitasdk"), "bin", "arm-vita-eabi-nm")
    starts = {}
    for line in subprocess.run([nm, "--defined-only", elf], capture_output=True, text=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "tT":
            starts[int(parts[0], 16) & ~1] = parts[2]
    return starts


def find_slide(addrs, starts):
    # Function pointers land exactly on symbol starts, so the right load offset matches them all
    first = min(addrs)
    best, score = 0, -1
    for start in starts:
        slide = first - start
        if slide & 0xfff:
            continue
        hits = sum(1 for a in addrs if (a - slide) in starts)
        if hits > score:
            best, score = slide, hits
            if hits == len(addrs):
                break
    return best


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    lines = open(sys.argv[1]).read().splitlines()
    starts = function_starts(sys.argv[2])
    addrs = {int(m.group(3), 16) & ~1 for m in map(FUNC.match, lines) if m}
    addrs |= {int(m.group(2), 16) & ~1 for m in map(ACTION.match, lines) if m}
    slide = find_slide(addrs, starts) if addrs else 0

    print(f"{'item':22} {'fps':>5} {'1%low':>6} {'hitch':>5} {'speed':>5} {'logic':>6} {'view':>6}")
    for m in filter(None, map(BENCH.match, lines)):
        g = m.groups()
        print(f"{g[0][6:]:22} {g[2]:>5} {g[4]:>6} {g[6]:>5} {g[7]:>4}% {g[8]:>6} {g[9]:>6}")
    print()
    current = None
    for line in lines:
        m = FUNC.match(line)
        if m:
            name = starts.get((int(m.group(3), 16) & ~1) - slide, m.group(3))
            line = f"{m.group(1)} func {m.group(2)} {name}: {m.group(4)}"
        m = ACTION.match(line)
        if m:
            name = starts.get((int(m.group(2), 16) & ~1) - slide, m.group(2))
            line = f"{m.group(1)} action {name}: {m.group(3)}"
        if any(k in line for k in (" type ", " func ", " action ", " checkpos: ", " lists per tic:")):
            label = re.split(r" (?:type|func|action|checkpos:|lists) ", line)[0]
            if label != current:
                print()
                current = label
            print(line)


if __name__ == "__main__":
    main()
