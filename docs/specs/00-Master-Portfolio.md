# Room Scanner Program: Master Portfolio
*Prepared November 12, 2026 · Index, shared contracts and spin-off catalog for Projects A to I*

## 1. What this set of documents is
The original plan describes one product: a phone-camera-only, voice-guided room scanner. This set splits it into nine independent projects, A to I, matching the parts of your diagram. Each project can ship on its own, can serve customers who never scan a room, and can produce its own technical or research achievements. Put together, they still assemble into the original scanner. Each project has its own development document.

## 2. The nine projects
| ID | Project | Standalone product | Headline achievement to pursue |
|---|---|---|---|
| A | CaptureKit | Cross-platform phone spatial-capture SDK and open scan format | Open Scan Bundle standard; thermal-aware capture |
| B | GuideMind | Real-time AI visual guidance and video-conversation engine | Planner-plus-phrasing architecture; Ask-the-Video; guided bots |
| C | SplatForge | Video-to-3D reconstruction service | Self-selecting pose path; metric-scale confidence |
| D | ScanQA | Automatic quality inspector and cleaner for 3D scans | Quality prediction without ground truth; machine-readable rescan requests |
| E | RoomSense | Room layout, object and measurement understanding | Floor plan and inventory from one walkthrough video |
| F | SplatView | Embeddable viewer, streaming and export | Hybrid splat plus mesh measurement viewer; narrated tours |
| G | ScanCloud | GPU-job platform for vision pipelines | Cost-capped, replayable pipelines; LLM gateway for mobile apps |
| H | ScanBench | Benchmarks, datasets, replay and simulation | Guidance-success metric; paired guided/unguided dataset |
| I | PrivacyShield | Privacy and security toolkit for home-scan data | View-consistent on-device redaction; deletion verification |

## 3. How they connect: shared contracts
Projects talk to each other only through versioned contracts, so every team can mock the others and work in parallel.

| Contract | Owner | Used by |
|---|---|---|
| Scan Bundle schema and validator | A | A, C, G, H |
| Coordinate convention spec plus tested conversion functions (ARKit, ARCore, COLMAP, OpenGL, splat tools) | A (shared geometry library) | All |
| PoseFrame, CoverageMap and Instruction types | B | A, B, F, H |
| QualityReport and weak-region schema | D | B, C, F, G |
| Job and stage manifest | G | C, D, E |
| Asset manifest (splat, mesh, thumbnails, LOD variants) | F | C, D, G |
| Consent flags and redaction manifest | I | A, C, F, G |

## 4. Suggested build order (waves)
- **Wave 0, day one:** H (benchmark rooms and replay tooling), I (threat model and data map), and the shared contracts above.
- **Wave 1, prove video becomes a model:** A (recorder, bundle, upload), G-lite (storage, queue, one GPU worker), C (feasibility test, then a one-worker pipeline), F-lite (basic viewer).
- **Wave 2, prove guidance helps:** B (coverage engine, rule-based planner, template voice), D (post-upload checks), H (guided versus unguided study).
- **Wave 3, productise:** I in full, G at scale, F polish, B conversational layer and Task Packs.
- **Wave 4, intelligence:** E (layout, objects, measurement).

This matches the source plan: steps 1 to 5 prove a phone video becomes a usable model; steps 6 and 7 prove guidance helps.

## 5. Headline spin-offs, one or two per project
- **A:** Open Scan Bundle format with converters; pre-flight capture scorer.
- **B:** Ask-the-Video assistant; guided-capture bots; Task Pack SDK.
- **C:** Video-to-3D API with scene presets; Pose Router library.
- **D:** Splat Doctor cleaner; scan quality score API.
- **E:** Floor plan and inventory from a video; will-it-fit checker.
- **F:** Embeddable viewer widget; narrated virtual tours.
- **G:** LLM and TTS gateway for mobile apps; cost-capped GPU pipeline runner.
- **H:** Public phone-only room benchmark; replay and simulation SDK.
- **I:** On-device scan anonymiser; deletion-verification tester.

## 6. Achievement catalog by type
- **Open standards:** Scan Bundle; Task Pack format; QualityReport schema; redaction manifest; coverage-metric specification.
- **Open-source libraries:** Coverage Engine; next-best-view planner and simulator; VoiceCoach; Pose Router; Splat Doctor; replay SDK; GPU pipeline runner; view-consistent redactor.
- **Datasets and benchmarks:** phone-only room reconstruction benchmark; paired guided/unguided capture dataset; failure-taxonomy dataset; phone scan-ability matrix.
- **Research candidates:** speech-budget study; ground-truth-free quality predictor; keyframe selection learned from downstream quality; counterfactual guidance evaluation; effect of privacy blurring on reconstruction; metric-scale confidence estimates; sonification versus speech guidance.
- **Products:** Ask-the-Video, guided bots, floor-plan-from-video, embeddable viewer, LLM gateway, anonymiser and the others listed in each project document.

## 7. A caution about calling something a first
The source plan says no verified project combines no-LiDAR capture, intelligent spoken guidance and full room reconstruction. That means none was found, not that none exists. Before any public claim of a first, run a prior-art search (papers, GitHub, app stores) and word it as the first we could find. Model and dataset licenses also need rechecking before commercial use, as the source plan notes.

## 8. Assumptions made in this set
- Both commercial and research outcomes stay possible, so designs avoid depending on non-commercial model weights where an alternative exists.
- iOS first for prototypes; Android arrives through Project A’s abstraction.
- Cloud reconstruction first (the source plan’s Option A), hybrid later.
- Small team per project; every numeric target in these documents is a proposal to tune, not a promise.

## 9. Open questions
1. Which project or spin-off should start first, and which are only ideas for later?
2. Who is the first customer: homeowners, real-estate agents, designers, builders, or developers who want the SDKs?
3. Is each project an app, an SDK, an API, or an open-source library?
4. Commercial, research, or both (decides which models and datasets are usable)?
5. Is offline operation a requirement for any project?
