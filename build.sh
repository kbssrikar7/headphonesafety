#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")"

APP_NAME="Headphone Safety"
BUNDLE_ID="com.local.headphonesafety"
BUILD_DIR=".build/release"
APP_DIR="$BUILD_DIR/${APP_NAME}.app"

# Build for both Apple Silicon and Intel and merge into one universal binary -
# a plain `swift build -c release` only produces a binary for the machine doing
# the build, which would silently make releases unusable on the other Mac
# architecture.
echo "Building (arm64)..."
swift build -c release --arch arm64
echo "Building (x86_64)..."
swift build -c release --arch x86_64

echo "Packaging ${APP_NAME}.app..."
rm -rf "$APP_DIR"
mkdir -p "$APP_DIR/Contents/MacOS"
mkdir -p "$APP_DIR/Contents/Resources"

lipo -create \
  ".build/arm64-apple-macosx/release/HeadphoneSafety" \
  ".build/x86_64-apple-macosx/release/HeadphoneSafety" \
  -output "$APP_DIR/Contents/MacOS/HeadphoneSafety"
cp Info.plist "$APP_DIR/Contents/Info.plist"
cp AppIcon.icns "$APP_DIR/Contents/Resources/AppIcon.icns"

echo "Ad-hoc code signing..."
codesign --force --deep --sign - "$APP_DIR"

echo ""
echo "Done. App bundle at:"
echo "  $(pwd)/$APP_DIR"
echo ""
echo "To install:"
echo "  cp -R \"$APP_DIR\" /Applications/"
echo ""
echo "To run now without installing:"
echo "  open \"$APP_DIR\""
