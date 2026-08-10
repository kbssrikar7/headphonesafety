#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")"

APP_NAME="Headphone Safety"
BUNDLE_ID="com.local.headphonesafety"
BUILD_DIR=".build/release"
APP_DIR="$BUILD_DIR/${APP_NAME}.app"

echo "Building..."
swift build -c release

echo "Packaging ${APP_NAME}.app..."
rm -rf "$APP_DIR"
mkdir -p "$APP_DIR/Contents/MacOS"
mkdir -p "$APP_DIR/Contents/Resources"

cp "$BUILD_DIR/HeadphoneSafety" "$APP_DIR/Contents/MacOS/HeadphoneSafety"
cp Info.plist "$APP_DIR/Contents/Info.plist"

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
