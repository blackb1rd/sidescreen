// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "spanly",
    platforms: [.macOS(.v14)],
    targets: [
        .target(name: "CGVirtualDisplay", path: "Sources/CGVirtualDisplay"),
        .systemLibrary(
            name: "CLibUSB",
            path: "Sources/CLibUSB",
            pkgConfig: "libusb-1.0",
            providers: [.brew(["libusb"])]
        ),
        .executableTarget(
            name: "spanly",
            dependencies: ["CGVirtualDisplay", "CLibUSB"],
            path: "Sources/spanly",
            linkerSettings: [
                .linkedFramework("ScreenCaptureKit"),
                .linkedFramework("VideoToolbox"),
            ]
        ),
        .testTarget(
            name: "SpanlyTests",
            dependencies: ["spanly"],
            path: "Tests/SpanlyTests"
        ),
    ],
    swiftLanguageModes: [.v5]
)
