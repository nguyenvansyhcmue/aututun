#!/usr/bin/env python3
"""Compare exported F0 contours and produce a compact, shareable SVG."""

import csv
import math
import sys
from pathlib import Path

SAMPLE_RATE = 48000.0
ALIGN_SECONDS = 0.500
VIEW_START = 3.80
VIEW_END = 4.62


def read_contour(path, shift=0.0):
    result = []
    with open(path, newline="") as source:
        for row in csv.DictReader(source):
            try:
                f0 = float(row["stable_f0_hz"])
            except ValueError:
                continue
            if f0 > 0.0 and math.isfinite(f0):
                result.append((float(row["sample"]) / SAMPLE_RATE + shift,
                               69.0 + 12.0 * math.log2(f0 / 440.0)))
    return result


def nearest_error_cents(midi):
    return 100.0 * (midi - round(midi))


def stats(points):
    selected = [(t, p) for t, p in points if VIEW_START <= t <= VIEW_END]
    errors = [nearest_error_cents(p) for _, p in selected]
    slews = []
    for (t0, p0), (t1, p1) in zip(selected, selected[1:]):
        if t1 > t0:
            slews.append(abs(100.0 * (p1 - p0) / (t1 - t0)))
    return {
        "frames": len(selected),
        "median_abs_error_cents": round(sorted(abs(x) for x in errors)[len(errors) // 2], 2) if errors else None,
        "within_5c_percent": round(100.0 * sum(abs(x) <= 5.0 for x in errors) / len(errors), 1) if errors else None,
        "peak_slew_cents_per_sec": round(max(slews), 1) if slews else None,
    }


def path(points, color, width, height):
    selected = [(t, p) for t, p in points if VIEW_START <= t <= VIEW_END]
    if not selected:
        return ""
    def xy(t, p):
        x = 60.0 + (t - VIEW_START) / (VIEW_END - VIEW_START) * (width - 80.0)
        y = 30.0 + (58.2 - p) / 2.8 * (height - 70.0)
        return x, y
    commands = []
    for i, (t, p) in enumerate(selected):
        x, y = xy(t, p)
        commands.append(("M" if i == 0 else "L") + f"{x:.1f},{y:.1f}")
    return f'<path d="{" ".join(commands)}" fill="none" stroke="{color}" stroke-width="2"/>'


def main():
    old = read_contour(sys.argv[1], ALIGN_SECONDS)
    new = read_contour(sys.argv[2], ALIGN_SECONDS)
    reference = read_contour(sys.argv[3])
    output = Path(sys.argv[4])
    width, height = 1200, 420
    grid = []
    for note in (56, 57, 58):
        y = 30.0 + (58.2 - note) / 2.8 * (height - 70.0)
        grid.append(f'<line x1="60" y1="{y:.1f}" x2="1120" y2="{y:.1f}" stroke="#555" stroke-dasharray="4 4"/>')
        grid.append(f'<text x="12" y="{y + 5:.1f}" fill="#bbb" font-size="14">MIDI {note}</text>')
    svg = f'''<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">
<rect width="100%" height="100%" fill="#151515"/>
<text x="60" y="20" fill="#eee" font-size="16">Rapid-legato F0 contour (candidate aligned +500 ms to reference)</text>
{''.join(grid)}
{path(reference, '#50d890', width, height)}
{path(old, '#e69f48', width, height)}
{path(new, '#4da3ff', width, height)}
<text x="760" y="45" fill="#50d890" font-size="14">reference</text>
<text x="860" y="45" fill="#e69f48" font-size="14">before</text>
<text x="950" y="45" fill="#4da3ff" font-size="14">new build</text>
<text x="60" y="405" fill="#aaa" font-size="13">time, seconds (reference timeline)</text>
</svg>'''
    output.write_text(svg)
    print("before", stats(old))
    print("new", stats(new))
    print("reference", stats(reference))


if __name__ == "__main__":
    main()
