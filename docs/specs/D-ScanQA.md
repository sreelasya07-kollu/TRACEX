# Project D: ScanQA
*Part D of the Room Scanner Program · Refinement and quality gate · Prepared November 12, 2026*

## 1. Pitch
An automatic quality inspector and cleaner for 3D scans. It takes a splat, a mesh and the camera poses, scores how trustworthy the result is, finds the weak regions, cleans the output, and can turn the weak regions into a machine-readable request to rescan. It works on scans from any source, not only the room scanner.

## 2. Why it stands alone
Most 3D capture tools leave the user to eyeball the result. Floaters, holes, drifted walls and wrong scale are found by hand, if at all. ScanQA answers one question for any scan: can I trust this, and where not? The source plan’s stages P8 (Cleanup) and P9 (Quality Gate) are the core, plus the feedback loop in your diagram: when the gate fails, a weak-region report goes back to Part B so the user can rescan.

## 3. Users and customers
- **Internal:** Project C output goes through the gate; Project B receives rescan requests; Project H uses the scores in benchmarks.
- **External:** anyone producing splats or meshes with Nerfstudio, gsplat or other tools; 3D marketplaces; real-estate and e-commerce teams that need a quality bar before publishing.

## 4. Scope
**In for v1:** floater removal; crop to room; floor alignment; hole filling; mesh simplification; held-out-view and geometric quality metrics; weak-region analysis; pass or fail decision; QualityReport.

**Out for v1:** semantic labelling (E), viewing (F), orchestration (G).

## 5. Components
| Component | Job | Tag |
|---|---|---|
| P8 Cleanup | Floater removal, crop to room, floor and wall alignment, hole filling, mesh simplification | Build, using Open3D and trimesh |
| P9 Quality Gate | Held-out-view PSNR, SSIM, LPIPS; reprojection error; coverage and hole analysis; pass or fail | Build, using torchmetrics and LPIPS |
| Weak-Region Analyser | Locates poorly reconstructed areas and explains why (few views, blank wall, glass, motion, drift) | Build |
| Rescan Request Generator | Converts weak regions into the RescanRequest and Region types that Project B understands | Build |
| Scale-Confidence Estimator | Reports how much to trust the metric scale | Build |
| QualityReport schema | Versioned output contract with a validator | Build |

## 6. Key design decisions
- **The report is machine-readable first, human-readable second.** Every weak region carries a reason code, a location and a priority.
- **Gate on usefulness, not just on a number.** A scan with a perfect average score and one wall missing should still fail.
- **Never hide uncertainty.** Output confidence per region, and per-scan scale confidence.
- **Deterministic checks before learned ones.** Learned predictors come later and are always compared against the deterministic baseline.

## 7. Spin-off products
1. **Splat Doctor:** open-source command-line and web tool that cleans any splat (floaters, crop, floor align, hole fill).
2. **Scan Quality Score API:** a single 0 to 100 score with plain explanations, usable from Nerfstudio-style pipelines.
3. **Rescan Request Generator:** weak regions as structured requests that any capture app can consume.
4. **Splat Diff:** compare two scans of the same room to show what changed (furniture moved, damage, renovation), reusing alignment from Project A’s re-scan kit.
5. **Collision Mesh Maker:** simplified meshes for AR placement, games and robots.
6. **Scan Confidence Badge:** a share-link badge that says how reliable the scale and geometry are, shown by Project F.
7. **Training-Data Curator:** filters bad scans out of datasets for Project H and for other teams.
8. **Failure Atlas:** a labelled collection of failure types (blank wall, mirror, ghosting, drift, blur) that teaches the gate and teaches users.
9. **Pipeline Regression Gate:** a CI check that fails a pipeline change if quality drops on a fixed set of scans.

## 8. Achievements to aim for (verify prior art before claiming a first)
- A quality predictor that estimates reconstruction quality without held-out ground truth, validated against tape-measured rooms and held-out views.
- A published failure taxonomy with labelled examples.
- Weak-region reports that close the loop with live guidance: measurable proof that a rescan request improves the next model.
- Per-region uncertainty maps for splats that users can see.
- A per-frame image-quality predictor (the source plan lists this as a later model) that tells capture which frames to keep.
- A public score definition other tools can adopt.

## 9. Roadmap
| Stage | Deliverable | Exit criterion (proposed) |
|---|---|---|
| D0 Definition | QualityReport schema, reason codes, pass or fail rules on paper | Reviewed with Projects B and C |
| D1 Cleanup basics | Floater removal, crop, floor alignment | Cleaned rooms look right on the benchmark set |
| D2 Metrics gate | Held-out-view metrics, reprojection error, hole analysis | Gate agrees with human judgement on a rated set |
| D3 Weak-region analyser | Reason-coded regions | Regions match what reviewers mark as bad |
| D4 Rescan requests | RescanRequest output wired to Project B | A rescan measurably improves the next model |
| D5 Scale confidence | Per-scan scale trust value | Confidence tracks real tape-measured error |
| D6 Learned predictor | Ground-truth-free quality estimate | Beats the deterministic baseline on held-out rooms |
| D7 Public tools | Splat Doctor, Score API, Failure Atlas | External users run them from the docs alone |

## 10. Metrics
Agreement between gate verdict and human ratings; false-pass and false-fail rates; weak-region precision and recall; rescan improvement (before versus after); scale confidence calibration; runtime and cost per scan.

## 11. Risks and mitigations
- **Over-aggressive cleanup removes real geometry:** keep originals, save a diff, and test against ground-truth rooms.
- **A single score hides problems:** always ship the region report beside the score.
- **Gate too strict, users stuck:** tune thresholds from remote config and track rescan rates.
- **Cleanup differs by scene type:** presets, as in Project C.

## 12. Interfaces with other projects
**In:** splat, mesh, poses and scale from C. **Out:** QualityReport and RescanRequest to B and G; cleaned assets to F and E; scores to H; confidence badge data to F.

## 13. Team and first 30 days
One 3D or computer vision engineer and a part-time evaluation engineer. Week 1: collect 20 scans, good and bad, and rate them by hand. Week 2: implement held-out-view metrics. Week 3: floater detection and floor alignment. Week 4: compare the gate to the hand ratings and fix the biggest disagreements.
