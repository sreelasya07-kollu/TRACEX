# Third-party versions

Pinned at repository setup (2026-11-12). Update deliberately: bump, rebuild, run CI, edit this file.

## C++ (git submodules under `third_party/`)

| Submodule | Upstream | Tag | Commit | Used as |
|---|---|---|---|---|
| `mcap` | https://github.com/foxglove/mcap.git | `releases/cpp/v2.2.0` | `9b4d3e36c7f7` | header-only, `cpp/mcap/include` (lz4/zstd disabled for now) |
| `eigen` | https://gitlab.com/libeigen/eigen.git | `5.0.1` | `bc3b39870ecb` | header-only |
| `sophus` | https://github.com/strasdat/Sophus.git | `1.24.6` | `d0b7315a0d90` | header-only (`sophus/`) |
| `nanobind` | https://github.com/wjakob/nanobind.git | `v3.1.0` | `68480a9e6883` | Python bindings (not built yet) |

Fetched by CMake: GoogleTest `v1.18.0` (FetchContent, tests only).

## Prebuilt (not committed)

| Artifact | Version | Fetched by |
|---|---|---|
| nihui/opencv-mobile | release `v38`, OpenCV `5.1.0` (ios, ios-simulator, android) | `scripts/fetch_opencv.sh` → `third_party/opencv-prebuilt/` |

## iOS (Swift Package Manager, `ios/Package.swift` + `ios/Package.resolved`)

| Package | Version | Product |
|---|---|---|
| tus/TUSKit | 3.7.0 (exact) | `TUSKit` |
| groue/GRDB.swift | 7.12.0 (exact) | `GRDB` |
| getsentry/sentry-cocoa | 9.34.0 (exact) | `Sentry` |

Requires Swift tools 6.1+ (Xcode 16.3+), forced by GRDB 7.12.

## Android (`android/gradle/libs.versions.toml`)

| Item | Version |
|---|---|
| Gradle wrapper | 9.9.1 |
| Android Gradle Plugin | 9.5.1 (built-in Kotlin) |
| Kotlin Gradle Plugin | 2.4.21 |
| KSP | 2.3.13 |
| compileSdk / targetSdk / minSdk | 37 / 37 / 26 |
| NDK / CMake (SDK) | 30.0.16248370 / 4.1.2 |
| com.google.ar:core | 1.57.0 |
| io.tus.android.client:tus-android-client | 0.1.12 |
| androidx.room (runtime, ktx, compiler) | 2.8.5 |
| androidx.work:work-runtime-ktx | 2.12.0 |
| io.sentry:sentry-android | 8.64.0 |
| junit:junit | 4.13.2 |

## Python (`tools/pyproject.toml`, locked in `tools/uv.lock`)

| Package | Locked |
|---|---|
| mcap | 1.6.0 |
| numpy | 2.5.4 (2.4.6 on Python 3.11, since numpy 2.5 needs ≥3.12) |
| opencv-python | 5.0.0.93 |
| pydantic | 2.14.3 |
| typer | 0.29.1 |
| jsonschema | 4.26.0 |
| pycolmap | 4.2.1 |
| nerfstudio (`[nerf]`) | 1.1.5 |
| pytest / ruff (`[dev]`) | 9.2.0 / 0.17.4 |

## Dev server images (`dev-server/docker-compose.yml`)

| Image | Tag |
|---|---|
| tusproject/tusd | v2.10.1 |
| pgsty/minio | RELEASE.2026-08-04T00-00-00Z |
| pgsty/mc | RELEASE.2026-08-04T00-00-00Z |

## Coordinate changes

- **MinIO**: `github.com/minio/minio` is archived and `minio/minio` / `minio/mc` are no longer
  pullable from Docker Hub (404 / "repository does not exist"); `quay.io/minio/*` needs auth.
  We use `pgsty/minio` and `pgsty/mc`, maintained community builds of the same AGPL code
  (same CLI, env vars and S3 API). Alternative: `cgr.dev/chainguard/minio` (only `:latest` is free).
  Dev-only; nothing in the SDK depends on MinIO specifically (any S3 API works with tusd).
- **opencv-mobile iOS archives** contain `opencv2.framework` at top level (Android archives
  are wrapped in a folder); `fetch_opencv.sh` handles both.
- No other repo or coordinate moves were found: all submodule URLs, SPM URLs, Maven
  coordinates and PyPI names in the setup brief resolved as given.
