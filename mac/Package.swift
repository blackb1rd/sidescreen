// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "sidescreen",
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
            name: "sidescreen",
            dependencies: ["CGVirtualDisplay", "CLibUSB"],
            path: "Sources/sidescreen",
            linkerSettings: [
                .linkedFramework("ScreenCaptureKit"),
                .linkedFramework("VideoToolbox"),
            ]
        ),
        .testTarget(
            name: "SideScreenTests",
            dependencies: ["sidescreen"],
            path: "Tests/SideScreenTests"
        ),
    ],
    swiftLanguageModes: [.v5]
)
