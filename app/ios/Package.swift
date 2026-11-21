// swift-tools-version:6.1
import PackageDescription
import Foundation

// The C++ core (static library for iOS, built by scripts/build_core_ios.sh) is linked only when
// CAPTUREKIT_CORE_LIB_DIR points at its folder. Without it the package builds exactly as before
// (Raw recorder only), so host CI keeps working. See ios/README.md.
let coreLibDir = ProcessInfo.processInfo.environment["CAPTUREKIT_CORE_LIB_DIR"]
let withCore = coreLibDir != nil
let coreTargets: [Target] = withCore ? [
    .target(
        name: "CCaptureKit",
        path: "Sources/CCaptureKit",
        cSettings: [.headerSearchPath("../../../core/include")],
        linkerSettings: [
            .unsafeFlags(["-L\(coreLibDir!)"]),
            .linkedLibrary("capturekit_core"),
            .linkedLibrary("c++"),
        ]
    ),
] : []

let package = Package(
    name: "CaptureKit",
    platforms: [
        .iOS(.v17),
        // macOS is declared only so `swift build` / `swift test` can run on a Mac host in CI.
        // The SDK itself targets iOS; ARKit/CoreMotion are linked on iOS only.
        .macOS(.v13),
    ],
    products: [
        .library(name: "CaptureKit", targets: ["CaptureKit"]),
    ],
    dependencies: [
        .package(url: "https://github.com/tus/TUSKit.git", exact: "3.7.0"),
        .package(url: "https://github.com/groue/GRDB.swift.git", exact: "7.12.0"),
        .package(url: "https://github.com/getsentry/sentry-cocoa.git", exact: "9.34.0"),
    ],
    targets: coreTargets + [
        .target(
            name: "CaptureKit",
            dependencies: (withCore ? ["CCaptureKit"] : []) + [
                .product(name: "TUSKit", package: "TUSKit"),
                .product(name: "GRDB", package: "GRDB.swift"),
                .product(name: "Sentry", package: "sentry-cocoa"),
            ],
            exclude: [
                "Session/README.md", "ARAdapter/README.md", "Camera/README.md",
                "Sensors/README.md", "Upload/README.md", "Platform/README.md", "UI/README.md",
            ],
            // Swift 5 mode: AR/CoreMotion delegate callbacks are not yet annotated for strict concurrency.
            swiftSettings: [.swiftLanguageMode(.v5)],
            linkerSettings: [
                .linkedFramework("ARKit", .when(platforms: [.iOS])),
                .linkedFramework("AVFoundation"),
                .linkedFramework("CoreMotion", .when(platforms: [.iOS])),
            ]
        ),
        .testTarget(
            name: "CaptureKitTests",
            dependencies: ["CaptureKit"]
        ),
    ]
)
