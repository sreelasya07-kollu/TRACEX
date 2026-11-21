# Project F: SplatView
*Part F of the Room Scanner Program · Viewer and export · Prepared November 12, 2026*

## 1. Pitch
An embeddable viewer, streaming layer and export toolkit for Gaussian splats and meshes. It loads quickly on a mid-range phone, lets people measure and annotate, shares safely, and converts to the formats other tools need. It is useful to anyone with a splat, not only room-scanner users.

## 2. Why it stands alone
Splat files are large, formats are fragmenting (PLY, SPZ, SOG, KSPLAT), and phones struggle with them. Existing renderers (three.js-based splat renderers, PlayCanvas and SuperSplat, Spark, native Metal and Vulkan renderers) solve parts of the problem. SplatView combines them with level-of-detail streaming, measurement, sharing and a clean embed, so a website, an app or a client can show a scan in one line of code.

## 3. Users and customers
- **Internal:** the room scanner’s web viewer and in-app viewer.
- **External:** real-estate and hotel sites, e-commerce, agencies, creators, other scanning apps.

## 4. Scope
**In for v1:** web viewer for splat and mesh; orbit and walk modes; share links; format converters; basic measurement; embed widget.

**Later:** native mobile renderer, streaming LOD, collaboration, WebXR, narrated tours.

## 5. Components
| Component | Job | Tag |
|---|---|---|
| Web Viewer | Renders splats and meshes in the browser (three.js splat renderer, PlayCanvas or Spark as the base) | Integrate |
| Embed Widget | One snippet that drops the viewer into any page | Build |
| Format Converters | PLY, SPZ, SOG, KSPLAT, GLB, OBJ, USDZ in and out | Integrate |
| Packager | Final assets, thumbnails and LOD variants (stage P10 in the source plan) | Integrate |
| LOD and Streaming Layer | Loads a coarse model fast, refines as data arrives | Build |
| Measurement Tool | Click two points on the mesh, show distance with uncertainty | Build |
| Annotation Layer | Pins, notes and region highlights from Projects D and E | Build |
| Sharing Service | Signed links, expiry, view counts, optional redaction regions | Build |
| Embedded Mobile Viewer | Native splat renderer or WebView inside the app | Integrate |

## 6. Key design decisions
- **Show the splat by default; measure on the mesh.** The viewer holds both and keeps them aligned.
- **Budget for a mid-range phone first.** Frame rate, memory and load time are product features.
- **Assets arrive through the asset manifest.** The viewer never guesses where files live.
- **Share safely by default:** private links, expiry, and respect for redaction regions from Project I.

## 7. Spin-off products
1. **Embeddable Scan Widget:** drop-in viewer for real-estate, hospitality and shop sites.
2. **Splat Converter and Compressor:** web tool and command-line utility, with size and quality comparison across formats.
3. **Progressive Splat Streaming:** coarse-to-fine loading for large scenes on phones.
4. **Measure and Annotate Overlay:** reusable measurement and note layer for any splat viewer.
5. **Narrated Tours:** an AI narrator walks through the model along a path, reusing Project B’s voice and Project E’s labels.
6. **Collaborative Review:** comments, pins and shared cameras, so a client and an agent can look at the same view.
7. **Before and After Viewer:** slider or side-by-side view of two scans, using Project D’s Splat Diff.
8. **WebXR and Headset Mode:** walk through the scan in VR or on an AR device.
9. **Redacted Sharing:** share a scan with chosen regions blurred or removed.
10. **Splat Renderer Benchmark:** a public comparison of renderers on real phones for load time, frame rate and memory.
11. **Accessible Viewer:** screen-reader descriptions and keyboard navigation, built from the layout and labels in Project E.

## 8. Achievements to aim for (verify prior art before claiming a first)
- A hybrid viewer that renders a splat while measuring on an aligned mesh, with uncertainty shown on every measurement.
- Progressive streaming that opens a room on a mid-range phone within a stated time budget.
- A public renderer benchmark on a fixed phone list.
- Redaction applied inside the viewer and in exports, verified by tests.
- Narrated tours generated from structured layout data with no manual scripting.

## 9. Roadmap
| Stage | Deliverable | Exit criterion (proposed) |
|---|---|---|
| F0 Evaluate | Test existing renderers on 3 to 5 phones with real scans | Pick a base renderer from measured results |
| F1 Web viewer MVP | Load, orbit, walk, share link | A scan from Project C opens on a phone and a laptop |
| F2 Converters | Format conversion and size report | Round trips preserve visual quality within a stated tolerance |
| F3 Measurement and annotations | Mesh-based distances, pins | Measurements match reference tape values within stated error |
| F4 Embed widget | One-snippet embed | A third-party page embeds a scan from the docs alone |
| F5 Streaming LOD | Coarse-to-fine load | Time to first view meets target on a mid-range phone |
| F6 Native mobile | In-app renderer | Frame rate target met on the device list |
| F7 Tours and collaboration | Narrated path, comments | A pilot user completes a client review |

## 10. Metrics
Time to first render; steady frame rate on reference phones; memory use; file size per quality level; measurement error; share-link usage; embed adoption.

## 11. Risks and mitigations
- **Large files on weak phones:** LOD and compression first.
- **Format churn:** keep converters modular and tested.
- **Splat and mesh drift apart:** align once in the packager and test.
- **Privacy leaks through links:** private by default, expiry, redaction, access logs.
- **Browser GPU differences:** test across browsers and graphics stacks.

## 12. Interfaces with other projects
**In:** asset manifest and splat, mesh, thumbnails (C, D); layout and labels (E); narration engine (B); redaction manifest (I). **Out:** view and share events to G; performance data to H.

## 13. Team and first 30 days
One web graphics engineer and part-time design help. Week 1: collect 5 scans of different sizes. Week 2: compare renderers on two phones and a laptop. Week 3: build the minimal viewer. Week 4: add share links and test redacted views.
