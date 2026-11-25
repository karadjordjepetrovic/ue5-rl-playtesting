#!/usr/bin/env python3
"""
score_run.py - score playtest runs against a test map's ground truth.

For each run it reports which planted bugs were found (an issue of an accepted type within the bug's
radius), how many issues fired inside control areas (false positives), and how many issues are
unexplained (not near any planted bug or control area).

Usage (from the project folder):
  python Plugins/PlaytestBots/Tools/score_run.py                                 # all runs in Saved/Playtest
  python Plugins/PlaytestBots/Tools/score_run.py Saved/Playtest/<runId>
  python Plugins/PlaytestBots/Tools/score_run.py Saved/Playtest --all            # every run of every scored map
  python Plugins/PlaytestBots/Tools/score_run.py Saved/Playtest --all --markdown # README-ready table

Ground truth is looked up as Plugins/PlaytestBots/TestMaps/<MapName>.ground_truth.json
(override with --ground-truth). Matching uses horizontal (XY) distance, because falls and
camera positions can differ a lot in height from the bug itself.

Location: Plugins/PlaytestBots/Tools/score_run.py
"""

import argparse
import json
import math
import os
import re
import sys
from collections import Counter, defaultdict

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_TRUTH_DIR = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "TestMaps"))
DEFAULT_RUNS_DIR = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "..", "..", "Saved", "Playtest"))


def xy_distance(a, b):
    return math.hypot(a[0] - b[0], a[1] - b[1])


def issue_location(issue):
    loc = issue.get("location") or issue.get("Location") or {}
    if isinstance(loc, dict):
        return (loc.get("x", loc.get("X", 0.0)), loc.get("y", loc.get("Y", 0.0)), loc.get("z", loc.get("Z", 0.0)))
    return tuple(loc)


def issue_type(issue):
    return issue.get("type") or issue.get("Type") or "Unknown"


def issue_time(issue):
    return issue.get("time", issue.get("Time", 0.0))


def map_name_from_run(run_dir):
    # runId = YYYYMMDD_HHMMSS_<MapName>
    match = re.match(r"^\d{8}_\d{6}_(.+)$", os.path.basename(os.path.normpath(run_dir)))
    return match.group(1) if match else None


def walker_from_config(config):
    match = re.search(r"walker=(\w+)", config or "")
    return match.group(1) if match else "?"


def bots_from_config(config):
    match = re.search(r"bots=(\d+)", config or "")
    return int(match.group(1)) if match else 0


def duration_from_config(config):
    match = re.search(r"duration=(\d+)", config or "")
    return int(match.group(1)) if match else 0


def score_run(run_dir, truth):
    report_path = os.path.join(run_dir, "report.json")
    if not os.path.isfile(report_path):
        return None
    with open(report_path, encoding="utf-8") as handle:
        report = json.load(handle)

    coverage = None
    coverage_path = os.path.join(run_dir, "coverage.json")
    if os.path.isfile(coverage_path):
        with open(coverage_path, encoding="utf-8") as handle:
            coverage = json.load(handle)

    bugs = truth.get("bugs", [])
    controls = truth.get("controls", [])

    bug_hits = defaultdict(list)       # bug id -> list of matching issues
    control_hits = defaultdict(list)   # control id -> issues inside it
    near_wrong_type = []               # near a bug but not an accepted type for it
    unexplained = []

    for issue in report.get("issues", []):
        loc = issue_location(issue)
        kind = issue_type(issue)

        matched_bug = False
        near_bug = False
        for bug in bugs:
            if xy_distance(loc, bug["location"]) <= bug["radius"]:
                near_bug = True
                accepted = bug.get("acceptedIssues") or [bug["expectedIssue"]]
                if kind in accepted:
                    bug_hits[bug["id"]].append(issue)
                    matched_bug = True
        if matched_bug:
            continue
        if near_bug:
            near_wrong_type.append(issue)
            continue

        in_control = False
        for control in controls:
            if xy_distance(loc, control["location"]) <= control["radius"]:
                control_hits[control["id"]].append(issue)
                in_control = True
                break
        if not in_control:
            unexplained.append(issue)

    config = report.get("config", "")
    return {
        "runId": report.get("runId", os.path.basename(run_dir)),
        "config": config,
        "walker": walker_from_config(config),
        "bots": bots_from_config(config),
        "duration": duration_from_config(config),
        "coveragePct": coverage.get("coveragePct") if coverage else None,
        "outsideNav": coverage.get("visitedOutsideNavCells") if coverage else None,
        "issueCount": len(report.get("issues", [])),
        "bugs": bugs,
        "bugHits": bug_hits,
        "controlHits": control_hits,
        "nearWrongType": near_wrong_type,
        "unexplained": unexplained,
    }


def cluster_issues(issues, size=200.0):
    groups = defaultdict(list)
    for issue in issues:
        x, y, z = issue_location(issue)
        key = (issue_type(issue), math.floor(x / size), math.floor(y / size))
        groups[key].append(issue)
    rows = []
    for (kind, _, _), group in groups.items():
        xs = [issue_location(i)[0] for i in group]
        ys = [issue_location(i)[1] for i in group]
        rows.append((len(group), kind, sum(xs) / len(xs), sum(ys) / len(ys)))
    return sorted(rows, reverse=True)


def print_run(result):
    print("=" * 78)
    print("Run      %s" % result["runId"])
    print("Config   %s" % result["config"])
    if result["coveragePct"] is not None:
        print("Coverage %.1f%%   outside navmesh: %s cells" % (result["coveragePct"], result["outsideNav"]))
    print("Issues   %d total" % result["issueCount"])
    print()
    print("  %-17s %-14s %-6s %5s %9s" % ("Planted bug", "Expected", "Found", "Hits", "First at"))
    found = 0
    for bug in result["bugs"]:
        hits = result["bugHits"].get(bug["id"], [])
        first = min((issue_time(i) for i in hits), default=None)
        if hits:
            found += 1
        accepted = "/".join(bug.get("acceptedIssues") or [bug["expectedIssue"]])
        print("  %-17s %-14s %-6s %5d %9s" % (
            bug["id"], accepted[:14], "YES" if hits else "no", len(hits),
            ("%.0f s" % first) if first is not None else "-"))
    print()
    control_fp = sum(len(v) for v in result["controlHits"].values())
    print("  Bugs found:            %d / %d" % (found, len(result["bugs"])))
    print("  Control false pos.:    %d" % control_fp)
    for control_id, issues in result["controlHits"].items():
        print("    %-20s %s" % (control_id, dict(Counter(issue_type(i) for i in issues))))
    print("  Near a bug, other type: %d %s" % (
        len(result["nearWrongType"]), dict(Counter(issue_type(i) for i in result["nearWrongType"]))))
    print("  Unexplained:           %d %s" % (
        len(result["unexplained"]), dict(Counter(issue_type(i) for i in result["unexplained"]))))
    for count, kind, x, y in cluster_issues(result["unexplained"])[:8]:
        print("    %3dx %-13s around (%.0f, %.0f)" % (count, kind, x, y))
    print()


def markdown_table(results):
    lines = [
        "| Run | Walker | Bots | Time | Coverage | Bugs found | Control FP | Unexplained |",
        "| --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for r in results:
        found = sum(1 for b in r["bugs"] if r["bugHits"].get(b["id"]))
        found_ids = ", ".join(b["id"] for b in r["bugs"] if r["bugHits"].get(b["id"]))
        lines.append("| %s | %s | %d | %d s | %s | %d/%d (%s) | %d | %d |" % (
            r["runId"], r["walker"], r["bots"], r["duration"],
            ("%.1f%%" % r["coveragePct"]) if r["coveragePct"] is not None else "-",
            found, len(r["bugs"]), found_ids or "none",
            sum(len(v) for v in r["controlHits"].values()), len(r["unexplained"])))
    return "\n".join(lines)


def load_truth(map_name, override):
    path = override or os.path.join(DEFAULT_TRUTH_DIR, "%s.ground_truth.json" % map_name)
    if not os.path.isfile(path):
        return None, path
    with open(path, encoding="utf-8") as handle:
        return json.load(handle), path


def main():
    parser = argparse.ArgumentParser(description="Score playtest runs against ground truth.")
    parser.add_argument("path", nargs="?", default=None,
                        help="A run folder, or with --all a folder of runs. Default: all runs in <Project>/Saved/Playtest")
    parser.add_argument("--all", action="store_true", help="Score every run under PATH that has ground truth")
    parser.add_argument("--ground-truth", help="Explicit ground_truth.json (default: by map name)")
    parser.add_argument("--markdown", action="store_true", help="Print a summary table in Markdown")
    args = parser.parse_args()
    if args.path is None:
        args.path = DEFAULT_RUNS_DIR
        args.all = True
    if not os.path.isdir(args.path):
        sys.exit("Folder not found: %s" % args.path)

    run_dirs = []
    if args.all:
        for name in sorted(os.listdir(args.path)):
            full = os.path.join(args.path, name)
            if os.path.isdir(full):
                run_dirs.append(full)
    else:
        run_dirs = [args.path]

    results = []
    for run_dir in run_dirs:
        map_name = map_name_from_run(run_dir)
        run_truth = os.path.join(run_dir, "ground_truth.json")  # written by procedural gyms
        truth, truth_path = load_truth(map_name, args.ground_truth or (run_truth if os.path.isfile(run_truth) else None))
        if truth is None:
            if not args.all:
                sys.exit("No ground truth for map '%s' (looked for %s)" % (map_name, truth_path))
            continue  # e.g. runs on maps without planted bugs
        result = score_run(run_dir, truth)
        if result is None:
            if not args.all:
                sys.exit("No report.json in %s (did the run finish? stopping PIE early skips the report)" % run_dir)
            continue
        results.append(result)
        if not args.markdown:
            print_run(result)

    if args.markdown and results:
        print(markdown_table(results))
    if not results:
        print("No scorable runs found.")


if __name__ == "__main__":
    main()
