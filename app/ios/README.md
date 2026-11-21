# ios

iOS SDK (Swift + ARKit), distributed as a Swift Package.

## Linking the C++ core (Phase 6, unverified on the dev machine)
`CaptureSession` talks to the core through the C API (`CCaptureKit`). The target is only added when the core
library is available, so plain `swift build` keeps working:
```sh
scripts/build_core_ios.sh                       # macOS only: libcapturekit_core.a for device + simulator
cd ios && CAPTUREKIT_CORE_LIB_DIR=$PWD/../build/ios-core/device swift build
```
