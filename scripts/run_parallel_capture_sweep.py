#!/usr/bin/env python3
"""Run isolated candidate captures concurrently and score them consistently.

The renderer/build command is supplied by the caller because Logic capture is
host-owned.  It receives one JSON config path and one output directory:

  --capture-command './capture_one.sh {config} {output_dir}'

``capture_one.sh`` must write ``logic_capture.csv`` to ``output_dir``.  This
tool never overwrites an installed AU or shares output directories.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import subprocess
import sys
from pathlib import Path


def run_one(candidate: dict, root: Path, command: str, scorer: Path) -> dict:
    identifier = str(candidate["id"])
    directory = root / identifier
    directory.mkdir(parents=True, exist_ok=False)
    config = directory / "candidate.json"
    config.write_text(json.dumps(candidate, indent=2) + "\n")
    rendered_command = command.format(config=str(config), output_dir=str(directory), id=identifier)
    capture = subprocess.run(rendered_command, shell=True, text=True, capture_output=True)
    (directory / "capture.stdout.log").write_text(capture.stdout)
    (directory / "capture.stderr.log").write_text(capture.stderr)
    if capture.returncode:
        return {"id": identifier, "status": "capture_failed", "returncode": capture.returncode}
    trace = directory / "logic_capture.csv"
    score = subprocess.run([sys.executable, str(scorer), str(trace), "--output-dir", str(directory / "score")],
                           text=True, capture_output=True)
    (directory / "score.stdout.log").write_text(score.stdout)
    (directory / "score.stderr.log").write_text(score.stderr)
    if score.returncode:
        return {"id": identifier, "status": "score_failed", "returncode": score.returncode}
    result = json.loads((directory / "score" / "summary.json").read_text())
    result.update({"id": identifier, "status": "scored", "parameter": candidate.get("parameter"),
                   "value": candidate.get("value")})
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=Path, help='JSON: {"candidates": [...]}')
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--capture-command", required=True)
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    candidates = json.loads(args.manifest.read_text()).get("candidates", [])
    if not candidates or any("id" not in candidate for candidate in candidates):
        raise SystemExit("manifest must contain non-empty candidates with unique id")
    ids = [candidate["id"] for candidate in candidates]
    if len(ids) != len(set(ids)):
        raise SystemExit("candidate ids must be unique")
    args.output_root.mkdir(parents=True, exist_ok=False)
    scorer = Path(__file__).with_name("score_grain_handoff_trace.py")
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        results = list(pool.map(lambda item: run_one(item, args.output_root, args.capture_command, scorer), candidates))
    scored = [row for row in results if row["status"] == "scored"]
    scored.sort(key=lambda row: (row["dry_guard_handoffs"] != 0 or row["grain_starvation_handoffs"] != 0,
                                 row["old_hold_p90_ms"], row["old_hold_max_ms"]))
    (args.output_root / "sweep_summary.json").write_text(json.dumps(scored, indent=2) + "\n")
    print(json.dumps(scored, indent=2))
    return 0 if len(scored) == len(candidates) else 1


if __name__ == "__main__":
    raise SystemExit(main())
