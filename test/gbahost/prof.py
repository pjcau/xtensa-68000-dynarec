#!/usr/bin/env python3
"""Symbolise the PROF lines of a gbahost run: `PROF=1 ./gbahost rom 600 | ./prof.py build/gbahost`."""
import bisect, re, subprocess, sys

binary = sys.argv[1] if len(sys.argv) > 1 else "build/gbahost"
syms = []
for line in subprocess.run(["nm", "-n", binary], capture_output=True, text=True).stdout.splitlines():
    f = line.split()
    if len(f) == 3 and f[1] in "tT":
        syms.append((int(f[0], 16), f[2]))
addrs = [a for a, _ in syms]
nm_main = dict(syms[::-1]) and next(a for a, n in syms if n == "_main")
base = None
raw, total = [], 0
lines = sys.stdin.read().splitlines()
for line in lines:
    m = re.match(r"PROF main (0x[0-9a-f]+)", line)
    if m:
        base = int(m.group(1), 16) - nm_main
assert base is not None, "no PROF main line (run with PROF=1)"
hits = {}
for line in lines:
    m = re.match(r"PROF (0x[0-9a-f]+) (\d+)", line)
    if m:
        pc, n = int(m.group(1), 16), int(m.group(2))
        i = bisect.bisect_right(addrs, pc - base) - 1
        name = syms[i][1] if i >= 0 else "?"
        hits[name] = hits.get(name, 0) + n
        total += n
    elif line.startswith("PROF total"):
        pass
for name, n in sorted(hits.items(), key=lambda kv: -kv[1])[:40]:
    print(f"{100.0 * n / total:6.2f}%  {n:7d}  {name}")
print(f"        {total:7d}  TOTAL")
