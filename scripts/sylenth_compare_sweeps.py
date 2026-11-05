#!/usr/bin/env python3
"""Side-by-side table of a sweep set measured on Sylenth1 and on Synthax1 (sylenth_measure.py JSON)."""
import json
import sys

sylenth, synthax1, keys = json.load(open(sys.argv[1])), json.load(open(sys.argv[2])), sys.argv[3].split(",")
for a, b in zip(sylenth, synthax1):
    cells = []
    for key in keys:
        x, y = a.get(key), b.get(key)
        if isinstance(x, (int, float)) and isinstance(y, (int, float)):
            cells.append(f"{key} {x:9.3g} | {y:9.3g}")
        else:
            cells.append(f"{key} {x!s:>9} | {y!s:>9}")
    print(f"{a['name']:24s}  " + "   ".join(cells))
