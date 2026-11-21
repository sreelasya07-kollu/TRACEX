# Project E: RoomSense
*Part E of the Room Scanner Program · Room and object understanding · Prepared November 12, 2026*

## 1. Pitch
A system that turns a reconstructed room into structured knowledge: floor, walls, ceiling, doors, windows, furniture and objects, with measurements and honest confidence values. From one walkthrough video it can produce a floor plan, an inventory and answers to spatial questions.

## 2. Why it stands alone
A pretty splat cannot answer how wide the door is, how much floor space is free, or what is in the room. RoomSense adds meaning to any 3D reconstruction. The source plan lists this as phase 3 and later, open-ended and high difficulty, so it stays out of the MVP, but it opens the largest set of new products. Prototypes to learn from (not depend on): Cansight and SpatialScene3D. Building blocks: SAM 2, open-vocabulary detectors, RANSAC plane fitting, Gaussian Grouping.

## 3. Users and customers
- Homeowners and renters (will it fit, how big is it).
- Real-estate agents (area, ceiling height, window count, floor plan).
- Interior designers and movers (inventory, clearances).
- Insurers (home inventory).
- Developers who want a room-understanding API.

## 4. Scope
**In for v1:** floor, wall and ceiling planes; door and window openings; room dimensions; basic object detection and masks; measurements with uncertainty; floor-plan export.

**Later:** object-level editing, IFC or DXF export, restyling, multi-room.

**Always out:** survey-grade accuracy. All measurement outputs carry a stated confidence and a not-for-construction notice.

## 5. Components
| Component | Job | Tag |
|---|---|---|
| Layout Extractor | Fits floor, wall and ceiling planes (RANSAC) and the room shell | Build |
| Opening Detector | Finds doors and windows in the shell | Build, using SAM 2 and detectors |
| Object Segmenter | Masks and labels furniture and objects from frames | Integrate |
| 2D-to-3D Lifter | Lifts masks into splats or meshes, giving object-level 3D | Integrate and Build |
| Measurement Engine | Distances, areas, volumes, each with uncertainty from scale confidence | Build |
| Confidence Scorer | Per-element confidence and reason codes | Build |
| Layout Exporter | Floor plan (SVG, PDF), JSON, later IFC and DXF | Build |
| Query Interface | Answers spatial questions from the structured model (shared with Project B) | Build |

## 6. Key design decisions
- **Structure first, labels second.** Planes and openings come from geometry; labels come from vision-language models and are never trusted without confidence.
- **Every number carries uncertainty.** Show 2.43 m plus or minus an error range, tied to the scale confidence from Projects C and D.
- **Work from the mesh for measurement and from the splat for looks.**
- **Keep outputs editable and open:** JSON first, because other tools can consume it.

## 7. Spin-off products
1. **Floor Plan from Video:** walk through, get a dimensioned floor plan.
2. **Will It Fit?** Enter furniture dimensions, check against the room, doors and corridors.
3. **Home Inventory:** object list with photos and positions, for insurance or moving quotes.
4. **Listing Auto-Details:** area, ceiling height, window and door counts for real-estate listings.
5. **Accessibility Survey Helper:** door widths and clear floor space reported as measurements for a human reviewer, not as a compliance verdict.
6. **Room Describer:** a spoken description of the room for low-vision users, reusing Project B’s voice layer.
7. **Wi-Fi and Sensor Placement Planner:** suggests router, camera or sensor positions from the layout.
8. **Egress and Safety Map:** exits and clear paths for planning, with a clear not-an-inspection notice.
9. **Robot-Ready Map:** floor-plan and obstacle grid exported for cleaning or delivery robots.
10. **Change Memory:** semantic diff between two scans (what moved, what was added), using Project D’s Splat Diff.
11. **Restyle and Remove:** object-level editing of splats with Gaussian Grouping so an object can be removed or replaced.
12. **Ask Your Room:** spatial questions answered from the model, built with Project B.

## 8. Achievements to aim for (verify prior art before claiming a first)
- Floor plan and inventory from a phone-only walkthrough, evaluated on tape-measured rooms.
- Measurements with calibrated uncertainty ranges, so a user can see which numbers to trust.
- Open-vocabulary object lifting into Gaussian splats, evaluated for 3D accuracy.
- A public benchmark for opening detection and layout accuracy on phone-only scans, built with Project H.
- A semantic diff for rooms: what changed between two visits.
- A published error budget showing how much each pipeline stage adds to measurement error.

## 9. Roadmap
| Stage | Deliverable | Exit criterion (proposed) |
|---|---|---|
| E0 Study | Review Cansight, SpatialScene3D and room-layout literature; collect rooms with measured ground truth | Target accuracy defined from user needs |
| E1 Layout from mesh | Plane fitting, room shell, dimensions | Dimensions within the stated error on reference rooms |
| E2 Openings | Door and window detection | Counts and sizes match ground truth on the benchmark |
| E3 Objects | Masks, labels and 3D lifting | Objects found and placed correctly on a labelled set |
| E4 Measurement with uncertainty | Engine plus confidence | Reported error ranges contain the true value at the stated rate |
| E5 Floor-plan export | SVG, PDF, JSON | Plans open correctly in common tools |
| E6 Products | Will It Fit, Inventory, Listing Details | A pilot user completes a task end to end |
| E7 Editing and IFC or DXF | Only if there is demand | Demand confirmed with real users |

## 10. Metrics
Dimension error in centimetres; opening detection precision and recall; object detection and 3D placement error; calibration of uncertainty ranges; user task completion; false label rate.

## 11. Risks and mitigations
- **Users read numbers as survey-grade:** show uncertainty and notices on every output.
- **Label errors in cluttered rooms:** show confidence and let users correct labels.
- **Mirrors and glass create phantom geometry:** consume exclusion flags from C and D.
- **Liability for accessibility, safety and measurement claims:** present measurements only, review wording with legal advisers.
- **Open-ended scope:** ship one product per stage; keep the MVP free of E.

## 12. Interfaces with other projects
**In:** cleaned splat, mesh, poses, scale and confidence (C, D); redaction rules (I). **Out:** layout, object and measurement JSON to F (annotations in the viewer) and B (spatial Q&A); benchmark entries to H.

## 13. Team and first 30 days
A 3D vision engineer with scene-understanding experience, plus a designer for the floor-plan outputs. Week 1: collect 10 rooms with tape-measured walls, doors and windows. Week 2: plane-fit the room shell from a mesh. Week 3: detect openings. Week 4: compare dimensions with the tape measurements and write the first error budget.
