# Models

| Model | HF id | Licence | Use |
|---|---|---|---|
| Depth-Anything-V2 Metric-Indoor **Small** (default) | `depth-anything/Depth-Anything-V2-Metric-Indoor-Small-hf` | **Apache-2.0** | Phase 3 mono depth (metric, Hypersim-trained) |
| Depth-Anything-V2 **Small** (relative) | `depth-anything/Depth-Anything-V2-Small-hf` | Apache-2.0 | Step A NYU reproduction; relative-depth variant |
| Depth-Anything-V2 Metric-Indoor Base / Large | `…-Metric-Indoor-Base-hf` / `…-Large-hf` | **CC-BY-NC-4.0** | opt-in only (`--model base|large`); not used for product defaults |

Licence source: Depth-Anything-V2 README ("Small model is under the Apache-2.0 license.
Base/Large/Giant models are under the CC-BY-NC-4.0 license."). The HF model cards carry no
licence field. The exact weights revision used is recorded per run in
`depth_alignment.json → model.revision`.

## Tools called as separate processes (docker)

| Tool | Licence (to verify before shipping) | How used |
|---|---|---|
| OpenMVS v2.4.0 (`room-openmvs:2.4.0`) | AGPL-3.0 (upstream licence; not independently verified here) | `InterfaceCOLMAP`, `DensifyPointCloud`, `ReconstructMesh`, `RefineMesh`, `TextureMesh` run as separate containers; no OpenMVS code is linked into this repo. Network-service use of AGPL code may trigger source-offer obligations: get legal review before offering Part 4 as a hosted API. |
| ETH3D multi-view-evaluation (`room-eth3d-eval`) | upstream licence not verified | scoring only (tests/benchmarks) |
| COLMAP / pycolmap 4.2.1 | BSD-3-Clause | library |
| Open3D 0.19.0 | MIT | library |
| gsplat 1.5.3 | Apache-2.0 | splat trainer (CUDA) |

Datasets used for evaluation (ETH3D CC BY-NC-SA 4.0, Replica research licence, TUM CC BY 4.0,
NYU research use) restrict commercial use: they are for internal evaluation only and must not
be redistributed or used to train a shipped model without a licence check.
