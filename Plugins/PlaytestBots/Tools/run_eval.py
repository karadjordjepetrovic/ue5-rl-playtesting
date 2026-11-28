"""
run_eval.py
Runs a batch of headless playtest runs (maps x walkers x seeds), scores each one and prints one Markdown table.

    py Plugins/PlaytestBots/Tools/run_eval.py                                  # held-out maps, all walkers, seeds 1-3
    py Plugins/PlaytestBots/Tools/run_eval.py --maps PT_GymTest --walkers Policy --model explorer_v4
    py Plugins/PlaytestBots/Tools/run_eval.py --score-only                     # just re-print the last batch

Each run uses -PTSeed=<seed> and -PTGymSeed=<seed>, so every walker sees the same procedural layouts.
Results are also saved to Saved/Playtest/eval_<timestamp>.json.

Location: Plugins/PlaytestBots/Tools/run_eval.py
"""

import argparse
import glob
import json
import os
import subprocess
import sys
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPT_DIR)
import score_run as sr  # noqa: E402

PROJECT_DIR = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "..", ".."))
RUNS_DIR = os.path.join(PROJECT_DIR, "Saved", "Playtest")
DEFAULT_UE = r"C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"


def find_uproject():
    found = glob.glob(os.path.join(PROJECT_DIR, "*.uproject"))
    if not found:
        sys.exit("No .uproject in " + PROJECT_DIR)
    return found[0]


def run_one(args, uproject, map_name, walker, seed):
    before = set(os.listdir(RUNS_DIR)) if os.path.isdir(RUNS_DIR) else set()
    cmd = [args.ue, uproject, "/Game/PlaytestBots/Maps/" + map_name, "-game", "-nullrhi", "-nosound", "-unattended",
           "-PTBots=%d" % args.bots, "-PTDuration=%d" % args.duration, "-PTWalker=" + walker,
           "-PTSeed=%d" % seed, "-PTGymSeed=%d" % seed, "-PTModel=" + args.model]
    started = time.time()
    print("  %-16s %-8s seed %d ... " % (map_name, walker, seed), end="", flush=True)
    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    new_dirs = sorted(d for d in set(os.listdir(RUNS_DIR)) - before if d.endswith("_" + map_name))
    if not new_dirs:
        print("no run folder (crashed?)")
        return None
    run_dir = os.path.join(RUNS_DIR, new_dirs[-1])
    print("%.0f s -> %s" % (time.time() - started, new_dirs[-1]))
    return run_dir


def score(run_dir, map_name, walker, seed):
    truth, _ = sr.load_truth(map_name, None)
    run_truth = os.path.join(run_dir, "ground_truth.json")
    if os.path.isfile(run_truth):
        with open(run_truth, encoding="utf-8") as handle:
            truth = json.load(handle)
    if truth is None:
        return None
    result = sr.score_run(run_dir, truth)
    if result is None:
        return None
    found = sum(1 for b in result["bugs"] if result["bugHits"].get(b["id"]))
    return {
        "map": map_name, "walker": walker, "seed": seed, "runDir": os.path.basename(run_dir),
        "coverage": result["coveragePct"], "found": found, "total": len(result["bugs"]),
        "foundIds": [b["id"] for b in result["bugs"] if result["bugHits"].get(b["id"])],
        "missedIds": [b["id"] for b in result["bugs"] if not result["bugHits"].get(b["id"])],
        "controlFp": sum(len(v) for v in result["controlHits"].values()),
        "unexplained": len(result["unexplained"]),
    }


def table(rows):
    lines = ["| Map | Walker | Coverage | Bugs found (per seed) | Total | Control FP | Unexplained |",
             "| --- | --- | --- | --- | --- | --- | --- |"]
    keys = []
    for r in rows:
        if (r["map"], r["walker"]) not in keys:
            keys.append((r["map"], r["walker"]))
    for map_name, walker in keys:
        group = [r for r in rows if r["map"] == map_name and r["walker"] == walker]
        covs = [r["coverage"] for r in group if r["coverage"] is not None]
        cov = ("%.1f-%.1f%%" % (min(covs), max(covs))) if covs else "-"
        per_seed = ", ".join("%d/%d" % (r["found"], r["total"]) for r in group)
        found, total = sum(r["found"] for r in group), sum(r["total"] for r in group)
        pct = (" (%.0f%%)" % (100.0 * found / total)) if total else ""
        fp = "-".join(sorted({str(r["controlFp"]) for r in group}, key=int))
        un = "-".join(sorted({str(r["unexplained"]) for r in group}, key=int))
        lines.append("| %s | %s | %s | %s | %d/%d%s | %s | %s |" % (map_name, walker, cov, per_seed, found, total, pct, fp, un))
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--maps", nargs="+", default=["PT_DetectorTest", "PT_HoldoutRooms"])
    parser.add_argument("--walkers", nargs="+", default=["Random", "Frontier", "Policy"])
    parser.add_argument("--seeds", nargs="+", type=int, default=[1, 2, 3])
    parser.add_argument("--model", default="explorer", help="Model folder under Saved/PlaytestModels (Policy walker)")
    parser.add_argument("--bots", type=int, default=16)
    parser.add_argument("--duration", type=int, default=180)
    parser.add_argument("--ue", default=DEFAULT_UE)
    parser.add_argument("--score-only", action="store_true", help="Re-print the most recent saved batch")
    args = parser.parse_args()

    if args.score_only:
        saved = sorted(glob.glob(os.path.join(RUNS_DIR, "eval_*.json")))
        if not saved:
            sys.exit("No saved batch in " + RUNS_DIR)
        with open(saved[-1], encoding="utf-8") as handle:
            batch = json.load(handle)
        print(table(batch["rows"]))
        return

    uproject = find_uproject()
    total_runs = len(args.maps) * len(args.walkers) * len(args.seeds)
    print("%d runs of %d s, model '%s' for Policy" % (total_runs, args.duration, args.model))
    rows = []
    for map_name in args.maps:
        for walker in args.walkers:
            for seed in args.seeds:
                run_dir = run_one(args, uproject, map_name, walker, seed)
                row = score(run_dir, map_name, walker, seed) if run_dir else None
                if row:
                    rows.append(row)

    stamp = time.strftime("%Y%m%d_%H%M%S")
    out = os.path.join(RUNS_DIR, "eval_%s.json" % stamp)
    with open(out, "w", encoding="utf-8") as handle:
        json.dump({"model": args.model, "bots": args.bots, "duration": args.duration, "rows": rows}, handle, indent=2)
    print()
    print(table(rows))
    print("\nSaved to " + out)


if __name__ == "__main__":
    main()
