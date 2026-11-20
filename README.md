# UE5 RL Playtesting Agents

Explorer bots for Unreal Engine 5 that cover a level automatically and report
level bugs: stuck spots, fall-throughs, out-of-bounds areas, collision holes
and performance hotspots. Bots are trained with UE5 Learning Agents (PPO) and
benchmarked against scripted walkers.

## Status

Phase 1 of 6: harness and scripted baselines (in progress)

| Phase | Goal | Status |
| --- | --- | --- |
| 1. Harness + baseline | Detectors, coverage grid, random and navmesh walkers, JSON report | In progress |
| 2. First RL agent | Learning Agents PPO explorer on one greybox level | Planned |
| 3. Procedural gym | Generalize to unseen levels | Planned |
| 4. Seeded bugs + report | Recall on planted bugs, HTML heatmap, replay | Planned |
| 5. Real level + CI | Sample-scale map, headless nightly run | Planned |
| 6. Write-up + demo | Results vs. baselines, video | Planned |

## Requirements

- Unreal Engine 5.8
- Visual Studio 2022 with "Game development with C++"
- Git LFS (`git lfs install` before cloning)

## Getting started

1. `git clone` the repo (LFS pulls the assets)
2. Right-click `PlaytestAgents.uproject` -> Generate Visual Studio project files
3. Build `Development Editor | Win64` and open the project

Headless run (available once Phase 1 is complete):


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

