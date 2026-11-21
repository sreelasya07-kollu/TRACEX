# Project H: ScanBench
*Part H of the Room Scanner Program · Evaluation and data · Prepared November 12, 2026*

## 1. Pitch
An open benchmark, dataset and simulation toolkit for phone-only indoor reconstruction and capture guidance. It records raw capture sessions so any change can be replayed, scores pipelines against tape-measured rooms, and measures the thing nobody measures yet: whether following an AI’s guidance actually produces a better model.

## 2. Why it stands alone
The source plan calls evaluation underrated and says it decides whether the project converges. Existing datasets (ScanNet++, ARKitScenes, ScanNet, Replica) were not built to test guidance, and most use depth sensors or laser scans. A phone-only, guidance-aware benchmark is useful to every team in this space, and it gives the room scanner an honest way to prove it works.

## 3. Users and customers
- **Internal:** every project (regression tests, benchmark numbers, guidance studies).
- **External:** researchers and companies building reconstruction or capture-guidance systems; students; tool makers who want a regression gate.

## 4. Scope
**In for v1:** a reference library of 30 to 50 rooms with tape-measured ground truth; raw session recording and replay; a benchmark runner; the metric suite; golden-scan regression tests; a guided-versus-unguided study kit.

**Later:** public leaderboard, synthetic rooms, simulated users, crowdsourced captures.

## 5. Components
| Component | Job | Tag |
|---|---|---|
| Reference Room Library | 30 to 50 rooms across lighting, clutter, blank walls and mirrors, with tape or laser ground truth | Build |
| Raw Session Recorder and Replay | Stores video, poses and IMU so any pipeline or planner can be rerun | Build (with Project A) |
| Benchmark Runner | Runs the pipeline on the library and produces a table of quality, scale error, runtime and cost | Build |
| Metric Suite | PSNR, SSIM, LPIPS, reprojection error, scale error, coverage completeness, guidance success rate | Build and Integrate |
| Guidance Simulator | Feeds recorded scans to the coverage engine and planner as if live | Build |
| Golden-Scan Regression Tests | Fixed scans with expected thresholds | Build |
| Internal Debug Viewer | Poses, coverage cells, planner decisions and spoken lines on a timeline | Build |
| Study Harness | Consent, screen and audio recording, questionnaires, guided versus unguided comparison | Build |
| Device Lab | A short list of real phones across price tiers for performance and heat | Buy and Build |
| Dataset and Experiment Tracking | Versioned datasets and experiment logs (DVC, MLflow or Weights and Biases) | Integrate |

## 6. Key design decisions
- **Record raw sessions from the first prototype.** A replayable scan is worth more than a live demo.
- **Ground truth by tape and laser, written down per room** (walls, doors, windows, a few object distances).
- **Keep a held-out set that development never touches.**
- **Consent first.** Rooms are people’s homes; every capture has consent, redaction and deletion terms (Project I).
- **Check dataset licenses** before training or commercial benchmarking; ScanNet++ and ARKitScenes have their own terms.

## 7. Spin-off products
1. **Phone-Only Room Reconstruction Benchmark** with a public leaderboard.
2. **Paired Guided and Unguided Capture Dataset:** the same people scanning the same rooms both ways.
3. **Replay and Simulation SDK:** replay any recorded session into any capture or guidance module.
4. **Synthetic Room Generator and Simulated Users:** make rooms with known geometry and virtual walkers, to train and stress-test planners.
5. **Failure Taxonomy Dataset:** labelled examples of blank-wall, mirror, ghosting, drift and blur failures (with Project D).
6. **Crowdsourced Capture Campaign:** volunteers record rooms with the Project A data collector app.
7. **Metrics Library for 3D Scans:** scale error, coverage and guidance-success metrics as a reusable package.
8. **Human-Factors Study Kit:** consent forms, questionnaires, recording setup and analysis scripts for capture-guidance studies.
9. **Pipeline Regression Action:** a CI step that runs a small fixed set of scans for any 3D pipeline.
10. **Device Lab as a Service:** performance and heat reports for other apps on the same phone list.

## 8. Achievements to aim for (verify prior art before claiming a first)
- Guidance success rate as a defined, reproducible metric: did following the prompts improve the final model?
- A paired guided and unguided capture dataset with tape-measured ground truth.
- A public benchmark that scores phone-only reconstruction by scale error in centimetres, not only by image metrics.
- A simulated-user model validated against real users, so planners can be tested without new field work.
- A pose-disagreement dataset comparing ARKit, ARCore and structure-from-motion (with Project A).
- A published speech-budget curve from Project B’s studies.

## 9. Roadmap
| Stage | Deliverable | Exit criterion (proposed) |
|---|---|---|
| H0 Protocol | Capture protocol, ground-truth sheet, metric definitions, consent text | Reviewed with Projects A, B and I |
| H1 First rooms | 10 rooms recorded, measured and stored | Protocol works for three different people |
| H2 Replay and runner | Replay tool and benchmark runner | A pipeline change produces a comparison table automatically |
| H3 Library of 30 to 50 rooms | Variety: lighting, clutter, blank walls, mirrors | Held-out split frozen |
| H4 Guidance simulator | Planner tested at a desk | Planner reaches target coverage in fewer prompts than a baseline in simulation |
| H5 Guided versus unguided study | Study harness and first results | Statistically useful comparison on a small group |
| H6 Public release | Benchmark, leaderboard, documentation | An outside team submits a result |
| H7 Synthetic and crowdsourced data | Generator, simulated users, data campaign | Simulated results track real-user results |

## 10. Metrics
Scale error (cm); held-out-view PSNR, SSIM, LPIPS; reprojection error; unattended success rate; coverage completeness; guidance success rate; time to scan; re-scan rate; user-reported frustration; runtime and cost per scan.

## 11. Risks and mitigations
- **Ground truth is slow to collect:** start with ten rooms and a short measurement sheet.
- **Privacy of home captures:** consent, redaction and deletion for every room.
- **Overfitting to the benchmark:** frozen held-out rooms and refreshed rooms every release.
- **Small user studies:** report uncertainty honestly; repeat studies as the product changes.
- **Dataset licensing:** check terms before any commercial use.

## 12. Interfaces with other projects
**In:** raw sessions and bundles (A); pipeline outputs (C, D, E); guidance logs (B); cost and runtime data (G). **Out:** benchmark tables and regression gates to every project; datasets to the research community.

## 13. Team and first 30 days
One evaluation engineer, one person who runs captures, and part-time design help for the study kit. Week 1: write the capture protocol and measurement sheet. Week 2: record and measure three rooms. Week 3: record raw sessions and run a first pipeline. Week 4: produce the first benchmark table and the first replay of a session through the coverage engine.
