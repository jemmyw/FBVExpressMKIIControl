// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "FBV",
    platforms: [.macOS(.v13)],
    products: [
        .executable(name: "fbv", targets: ["fbv"]),
        .executable(name: "FBVApp", targets: ["FBVApp"]),
        .library(name: "PedalKit", targets: ["PedalKit"])
    ],
    targets: [
        .target(name: "FBVCore", linkerSettings: [
            .linkedFramework("CoreMIDI"), .linkedFramework("CoreFoundation")
        ]),
        .executableTarget(name: "fbv", dependencies: ["FBVCore"]),
        .target(name: "PedalKit", dependencies: ["FBVCore"]),
        .executableTarget(name: "FBVApp", dependencies: ["PedalKit"]),
        .testTarget(name: "PedalKitTests", dependencies: ["PedalKit", "FBVCore"])
    ],
    cLanguageStandard: .c11
)
