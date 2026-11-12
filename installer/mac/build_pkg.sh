#!/bin/bash
# Builds a macOS installer: OpenTrigger-<version>-macOS.pkg
#   usage: build_pkg.sh <version label, e.g. 0.1.0-beta.1> <folder with VST3/ AU/ Standalone/ built artefacts> <output folder>
# The package is NOT signed or notarized (that needs an Apple Developer ID); the postinstall script removes the
# quarantine flag so macOS does not block the plug-ins on first load.
set -euo pipefail
VERSION="${1:?version}"; ART="${2:?artefacts folder}"; OUT="${3:?output folder}"
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT" "$WORK/vst3" "$WORK/au" "$WORK/app"
cp -R "$ART/VST3/OpenTrigger.vst3" "$WORK/vst3/"
cp -R "$ART/AU/OpenTrigger.component" "$WORK/au/"
cp -R "$ART/Standalone/OpenTrigger.app" "$WORK/app/"
IDV="com.opentrigger"
pkgbuild --root "$WORK/vst3" --identifier "$IDV.vst3" --version "$VERSION" --install-location "/Library/Audio/Plug-Ins/VST3" "$WORK/vst3.pkg"
pkgbuild --root "$WORK/au"   --identifier "$IDV.au"   --version "$VERSION" --install-location "/Library/Audio/Plug-Ins/Components" "$WORK/au.pkg"
pkgbuild --root "$WORK/app"  --identifier "$IDV.app"  --version "$VERSION" --install-location "/Applications" --scripts "$HERE/scripts" "$WORK/app.pkg"
sed "s/@VERSION@/$VERSION/g" "$HERE/distribution.xml" > "$WORK/distribution.xml"
productbuild --distribution "$WORK/distribution.xml" --package-path "$WORK" --version "$VERSION" "$OUT/OpenTrigger-$VERSION-macOS.pkg"
echo "built $OUT/OpenTrigger-$VERSION-macOS.pkg"
