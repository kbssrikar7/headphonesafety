// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "HeadphoneSafety",
    platforms: [.macOS(.v12)],
    targets: [
        .executableTarget(
            name: "HeadphoneSafety",
            path: "Sources/HeadphoneSafety"
        )
    ]
)
