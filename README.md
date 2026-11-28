# UE5 RL Playtesting Agents

Explorer bots for Unreal Engine 5 that cover a level automatically and report
level bugs: stuck spots, fall-throughs, out-of-bounds areas, collision holes
and performance hotspots. Bots are trained with UE5 Learning Agents (PPO) and
benchmarked against scripted walkers.

## Status

Phase 3 complete: bots trained on procedurally generated levels find more planted bugs than scripted bots on a held-out map they never trained on. Next: a run on a real-sized level, then the write-up. Phases 4 and 5 are reduced to that one real-level run.

| Phase | Goal | Status |
| --- | --- | --- |
| 1. Harness + baseline | Detectors, coverage grid, random and navmesh walkers, JSON report | Done (`v0.1-phase1`) |
| 2. First RL agent | Learning Agents PPO explorer on one greybox level | Done (`v0.2-phase2`) |
| 3. Procedural gym | Generalize to unseen levels | Done (`v0.3-phase3`) |
| 4. Seeded bugs + report | Recall on planted bugs, HTML heatmap, replay | Dropped (recall scoring done in Phases 1-3) |
| 5. Real level + CI | Sample-scale map, headless nightly run | Next: real-sized level only |
| 6. Write-up + demo | Results vs. baselines, video | Planned |

## Requirements

- Unreal Engine 5.8
- Visual Studio 2022 with "Game development with C++"
- Git LFS (`git lfs install` before cloning)

## Getting started

1. `git clone` the repo (LFS pulls the assets)
2. Right-click `PlaytestAgents.uproject` -> Generate Visual Studio project files
3. Build `Development Editor | Win64` and open the project

Headless evaluation run (16 bots, 3 minutes, trained policy):

```powershell
& "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "D:\dev\PlaytestAgents\PlaytestAgents.uproject" /Game/PlaytestBots/Maps/PT_DetectorTest -game -nullrhi -unattended -PTBots=16 -PTDuration=180 -PTWalker=Policy "-PTSeed=1" -log
```

Options: `-PTWalker=Random|Frontier|Policy`, `-PTModel=<name>` (a folder under `Saved/PlaytestModels`), `-PTTrain` to train, `-PTFresh` to start from an untrained network. In PIE: `pt.Start 16 180 Policy 1`.

Scoring and training tools:

- `py Plugins/PlaytestBots/Tools/score_run.py` scores runs in `Saved/Playtest` against the map's `ground_truth.json`
- `py Plugins/PlaytestBots/Tools/training_curve.py` summarizes the last training log and flags a critic collapse


## Layout

- `Plugins/PlaytestBots/` - all project code (portable to other UE projects)
- `Content/PlaytestBots/Maps/` - test maps
- `Tools/report/` - report tooling (Python)

## Baseline results

Headless runs on `PT_DetectorTest`, 16 bots, 180 s, 3 seeds per walker (perf not detectable headless, so bugs out of 5).

| Walker | Coverage | Bugs found | Control FP | Unexplained |
| --- | --- | --- | --- | --- |
| Random | 91.7-95.5% | 3/5 every seed (floor hole, edge gap, clip wall) | 0 | 0-3 |
| Frontier | 98.9-99.2% | 1/5 every seed (clip wall) | 0 | 0 |

Neither baseline finds the stuck shaft or the jump-only ledge; that is the target for the RL agent in Phase 2.

## Phase 2 results

Same setup (16 bots, 180 s, seeds 1-3, bugs out of 5). Model `explorer_v4`: trained on `PT_DetectorTest`, 32 bots.

| Walker | Coverage | Bugs found | Control FP | Unexplained |
| --- | --- | --- | --- | --- |
| Random | 91.7-95.5% | 3, 3, 3 | 0 | 0-3 |
| Frontier | 98.9-99.2% | 1, 1, 1 | 0 | 0 |
| **Policy** | **99.4-99.9%** | **4, 3, 4** (floor hole, edge gap, clip wall; stuck shaft in 2 of 3) | **0** | **0** |

The policy finds the stuck shaft, which neither baseline ever did, at Frontier-level coverage. The jump-only ledge is still not found. Key fixes on the way: per-episode bug rewards, in-episode relocation after falls (truncation caused a critic collapse), a jump sensor with a jump cost, and novelty scaled by time since any bot visited a cell (removed a wall-following loop).

## Phase 3 results

Bots train on 4 procedural gyms at once (`PT_GymTest`: 8 x 8 tiles of 4 m, platforms, ramps, stairs, ledges, low ceilings, 2-4 planted bugs per layout, a new layout every 10 minutes). Evaluation: 16 bots, 180 s, seeds 1-3, `Tools/run_eval.py`. `PT_HoldoutRooms` (rooms, doorways, a roof deck) is never used for training.

| Map | Random | Frontier | Policy `explorer_v4` (one map) | **Policy `explorer_gym2` (gyms)** |
| --- | --- | --- | --- | --- |
| `PT_HoldoutRooms` bugs | 7/15 (47%) | 6/15 (40%) | 9/15 (60%) | **11/15 (73%)** |
| `PT_HoldoutRooms` coverage | 63-65% | 91-92% | 94-98% | **97-98%** |
| `PT_DetectorTest` bugs (of 5 per seed) | 11/15 | 3/15 | 12/15 | **12/15** |
| `PT_GymTest` bugs | **20/33 (61%)** | 3/33 (9%) | 13/33 (39%) | 17/33 (52%) |

0 control false positives and 0 unexplained issues for every policy run. Random still finds the most bugs inside the small, hazard-dense gyms (it falls into everything), but on the held-out rooms map the gym-trained policy finds the most. It is also the first walker to find the jump-only forbidden ledge on `PT_DetectorTest` (2 of 3 runs). Weak spot: stuck shafts in the gyms.

Key lessons: falls must not end the episode (a truncation there made the critic diverge, a training collapse); novelty has to be scaled by time since any bot visited a cell (otherwise bots learn to run laps along walls); the fall bonus is paid once per bot per fall spot per layout (a once-per-3-minutes rule made the bots too cautious).
