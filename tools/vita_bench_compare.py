#!/usr/bin/env python3
"""Compare Vita benchmark results map by map (fps, game speed, logic ms/tic).

usage: vita_bench_compare.py BASE.txt NEW.txt [MORE.txt ...]
"""
import re
import sys

LINE = re.compile(r"^Bench ([^:]+): .*?, ([\d.]+) fps, .*game speed (\d+)%.*\| tic: logic ([\d.]+) ")


def load(path):
    out = {}
    for line in open(path):
        m = LINE.match(line)
        if m:
            out[m.group(1)] = (float(m.group(2)), int(m.group(3)), float(m.group(4)))
    return out


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    runs = [load(p) for p in sys.argv[1:]]
    names = [p.rsplit("/", 1)[-1].removeprefix("result-").removesuffix(".txt") for p in sys.argv[1:]]
    head = f"{'map':8}" + "".join(f" {n[:14]:>22}" for n in names)
    print(head)
    print(f"{'':8}" + "".join(f" {'fps spd logic':>22}" for _ in names))
    totals = [0.0] * len(runs)
    for item in runs[0]:
        cells = []
        for i, run in enumerate(runs):
            if item not in run:
                cells.append(f" {'-':>22}")
                continue
            fps, speed, logic = run[item]
            totals[i] += logic
            delta = "" if i == 0 else f"{(logic - runs[0][item][2]) / runs[0][item][2] * 100:+.0f}%"
            cells.append(f" {fps:5.1f} {speed:3d}% {logic:5.1f} {delta:>5}")
        print(f"{item:8}" + "".join(cells))
    print(f"{'logic sum':8}" + "".join(f" {t:22.1f}" for t in totals))


if __name__ == "__main__":
    main()
