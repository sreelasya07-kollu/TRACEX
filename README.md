# TRACEX — 2D-to-3D Spatial Room Reconstruction Platform

TRACEX is an end-to-end intelligent room scanning and 3D reconstruction system organized into clean functional modules:

```
TRACEX/
├── app/                  # Mobile capture applications (Android Kotlin & iOS Swift)
├── core/                 # C++20 geometry core, thermal governor, and frame quality engine
├── recon/                # 3D Reconstruction pipeline, TSDF fusion & 3D Gaussian Splatting
├── tools/                # CLI validation, pipeline router, benchmarks & evaluation suites
├── docs/                 # Specifications, ADRs, and technical documentation
└── .github/              # Continuous integration workflows
```

---

## Functional Architecture

- **`app/`**:
  - `app/android/`: Native Android capture application (Kotlin + Jetpack Compose + ARCore).
  - `app/ios/`: Native iOS capture application (Swift + ARKit).
- **`core/`**:
  - C++20 core library (`geometry`, `governor`, `quality`, `bundle`, `bindings`).
  - Native JNI, Python, and Swift bindings.
- **`recon/`**:
  - `spatial/`: Scan bundle ingest, pose refinement, sensor depth filtering, TSDF volumetric fusion, plane/room geometry.
  - `recon/`: OpenMVS dense point cloud meshing, texturing, and 3D Gaussian Splatting (`gsplat`).
  - `formats/`: Canonical Pydantic schemas (`ScanBundle`, `SpatialMap`, `ReconstructionManifest`).
  - `export/`: USD, point cloud, and floor plan export engines.
  - `qa/`: Quality gates, coverage analysis, and threshold validation.
- **`tools/`**:
  - `ckit/`: Client bundle validator, replay simulation, and trial reporter.
  - `router/`: Pipeline execution orchestrator.
  - `eval/`: Synthetic room generator and Chamfer distance metrics.
  - `benchmarks/`: NYU, TUM, and ETH3D evaluation suites.

---

## Quickstart

### Mobile App (`app/android/`)
```bash
cd app/android
./gradlew assembleStandardDebug
```

### Reconstruction Engine (`recon/`)
```bash
cd recon
uv sync --extra cpu    # or --extra cu126
uv run pytest
```

### Scan Bundle Validator (`tools/ckit/`)
```bash
cd tools/ckit
uv run ckit --help
```
