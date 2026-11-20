"""Checks the bundled layouts' key geometry: usage layout_geometry_tests.py LAYOUTS_DIR.

Rows are as far apart as keys in a row are: 6 layout units between a key and
the next key below it in its column, or a whole number of rows more where a
row is empty there, as between the navigation keys and the arrows. Under a
function-row key, 44 tall at the top, the gap is 22. No two keys overlap and
none reaches past the layout's width or height.
"""

import json
import sys
from pathlib import Path

GAP = 6
KEY = 62
PITCH = KEY + GAP
FUNCTION_GAP = 22
TOLERANCE = 0.01
# No key keeps another gap (R344 made the flick English page's emoji key as
# tall as the side keys under it).
KEPT_GAPS = {}


def problems(path):
    layout = json.loads(path.read_text(encoding="utf-8"))
    keys = layout["keys"]
    width, height = layout["width"], layout["height"]
    found = []
    for key in keys:
        if key["x"] < -TOLERANCE or key["y"] < -TOLERANCE:
            found.append(f"{key['id']} starts outside the layout")
        if key["x"] + key["width"] > width + TOLERANCE or key["y"] + key["height"] > height + TOLERANCE:
            found.append(f"{key['id']} reaches past the layout's width or height")
    for i, a in enumerate(keys):
        for b in keys[i + 1 :]:
            across = min(a["x"] + a["width"], b["x"] + b["width"]) - max(a["x"], b["x"])
            down = min(a["y"] + a["height"], b["y"] + b["height"]) - max(a["y"], b["y"])
            if across > TOLERANCE and down > TOLERANCE:
                found.append(f"{a['id']} and {b['id']} overlap")
    for upper in keys:
        below = [
            key
            for key in keys
            if key is not upper
            and key["y"] >= upper["y"] + upper["height"] - TOLERANCE
            and min(key["x"] + key["width"], upper["x"] + upper["width"]) - max(key["x"], upper["x"]) > 1
        ]
        if not below:
            continue
        gap = min(key["y"] for key in below) - (upper["y"] + upper["height"])
        kept = KEPT_GAPS.get((path.stem, upper["id"]))
        if kept is not None:
            allowed = [kept]
        elif upper["y"] < TOLERANCE and upper["height"] == 44:
            allowed = [FUNCTION_GAP]
        else:
            allowed = [GAP + PITCH * rows for rows in range(6)]
        if not any(abs(gap - value) < TOLERANCE for value in allowed):
            found.append(f"{upper['id']}: {gap:g} units to the key below, not {allowed[0]:g}")
    return found


def main():
    folder = Path(sys.argv[1])
    paths = sorted(folder.glob("*.json"))
    if not paths:
        print(f"no layouts in {folder}")
        return 1
    failed = False
    for path in paths:
        for problem in problems(path):
            print(f"{path.name}: {problem}")
            failed = True
    if failed:
        return 1
    print(f"{len(paths)} layouts: rows 6 units apart, no overlaps, all within their size")
    return 0


if __name__ == "__main__":
    sys.exit(main())
