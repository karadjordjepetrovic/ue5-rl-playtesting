"""Summarise a Learning Agents training run from the Unreal log and warn about a critic collapse.

Usage (from the project folder):
    py Plugins/PlaytestBots/Tools/training_curve.py                 # Saved/Logs/PlaytestAgents.log
    py Plugins/PlaytestBots/Tools/training_curve.py path/to/log --slices 12

Reads two kinds of lines:
  - our progress line (LogPlaytestReward): mean reward and new cells per step
  - the PPO trainer's line: "Iter: N | Avg Rew: ... | Avg Return: ..."
"Avg Return" is the discounted return target the critic learns. With per-step rewards around 0.1-0.2 it should
stay small and positive (roughly 5-25). A steady slide to large negative values while Avg Rew is still fine
means the critic is diverging; the policy usually collapses soon after. Use the last checkpoint before it.
"""
import argparse
import re
from pathlib import Path

ITER = re.compile(r"Iter:\s+(\d+) \| Avg Rew: ([-\d.]+) \| Avg Rew Sum: ([-\d.]+) \| Avg Return: ([-\d.]+)")
OURS = re.compile(r"mean reward ([-\d.]+), new cells/step ([\d.]+), issues (\d+)")
CKPT = re.compile(r"Checkpoint saved to (.+)")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log", nargs="?", help="Default: newest log in Saved/Logs that has trainer lines "
                        "(any later launch moves the training log to a -backup- file)")
    parser.add_argument("--slices", type=int, default=12)
    args = parser.parse_args()
    if not args.log:
        logs_dir = Path(__file__).resolve().parents[3] / "Saved" / "Logs"
        candidates = sorted(logs_dir.glob("PlaytestAgents*.log"), key=lambda p: p.stat().st_mtime, reverse=True)
        args.log = next((str(p) for p in candidates if "Iter:" in p.read_text(errors="ignore")), None)
        if not args.log:
            print("No log with trainer lines found in", logs_dir)
            return
    print("Log:", args.log)

    iters, ours, checkpoints = [], [], []
    for line in open(args.log, errors="ignore"):
        if m := ITER.search(line):
            iters.append((int(m[1]), float(m[2]), float(m[4])))
        elif m := OURS.search(line):
            ours.append((float(m[1]), float(m[2])))
        elif m := CKPT.search(line):
            checkpoints.append((len(iters), m[1].strip()))

    if not iters:
        print("No trainer lines found in", args.log)
        return

    n = len(iters)
    size = max(1, n // args.slices)
    print(f"{'iters':>13} {'avg rew':>8} {'avg return':>11} {'new cells/step':>15}")
    best_return, collapse_at = None, None
    for start in range(0, n, size):
        chunk = iters[start:start + size]
        rew = sum(c[1] for c in chunk) / len(chunk)
        ret = sum(c[2] for c in chunk) / len(chunk)
        o0, o1 = start * len(ours) // n, (start + len(chunk)) * len(ours) // n
        ochunk = ours[o0:o1]
        cells = f"{sum(c[1] for c in ochunk) / len(ochunk):.3f}" if ochunk else "-"
        flag = ""
        if best_return is None or ret > best_return:
            best_return = ret
        elif ret < min(0.0, best_return * 0.25) and collapse_at is None:
            collapse_at = chunk[0][0]
            flag = "  <-- critic diverging"
        print(f"{chunk[0][0]:>6}-{chunk[-1][0]:<6} {rew:8.3f} {ret:11.2f} {cells:>15}{flag}")

    if collapse_at is None:
        print("\nNo collapse detected.")
    else:
        print(f"\nWARNING: Avg Return fell below zero from iteration ~{collapse_at}. "
              "Evaluate a checkpoint from before that point.")
    if checkpoints:
        print("\nCheckpoints (trainer iteration reached when saved):")
        for idx, path in checkpoints:
            it = iters[min(idx, n) - 1][0] if idx else 0
            print(f"  iter ~{it:>6}  {path}")


if __name__ == "__main__":
    main()
