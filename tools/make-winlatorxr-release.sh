#!/usr/bin/env bash
# Packs the WinlatorXR (standalone Quest) release ZIP from a finished build.
# Usage: tools/make-winlatorxr-release.sh <version> <RF.exe 1.21 GOG> <RF_120na.exe> [build dir]
# The 1.21 -> 1.20 NA executable patch is generated with minibsdiff from the
# two given executables so users only need their own RF.exe.
set -euo pipefail

version=${1:?version, e.g. 0.1.0-beta}
rf121=${2:?path to the GOG RF.exe (1.21)}
rf120=${3:?path to RF_120na.exe (1.20 NA)}
repo=$(cd "$(dirname "$0")/.." && pwd)
bin=${4:-$repo/build-mingw/bin}
out=${OUT_DIR:-$HOME/Projekte/alpinefactionVR-releases}
name=RedFactionVR-Quest-WinlatorXR-v$version
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT

pkg=$stage/RedFaction
mkdir -p "$pkg/mods/VR" "$out"
for f in AlpineFactionVR.exe AlpineFaction.dll CrashHandler.exe d3d8to9.dll alpinefaction.vpp licensing-info.txt minibsdiff.exe; do
    cp "$bin/$f" "$pkg/"
done
cp "$repo/resources/vr_mod/vr_weapons.vpp" "$pkg/mods/VR/"
cp "$repo/resources/winlatorxr/"{RedFactionVR.bat,afvr_winlatorxr.reg,afvr_winlatorxr.ini,README-WinlatorXR.txt} "$pkg/"
cp "$repo/LICENSE.txt" "$pkg/"

WINEDEBUG=-all wine "$bin/minibsdiff.exe" gen "$rf121" "$rf120" "$pkg/RF-1.21.exe.mbsdiff"
cp "$rf121" "$stage/check-src.exe"
WINEDEBUG=-all wine "$bin/minibsdiff.exe" app "$stage/check-src.exe" "$pkg/RF-1.21.exe.mbsdiff" "$stage/check.exe"
cmp "$stage/check.exe" "$rf120"

rm -f "$out/$name.zip"
(cd "$pkg" && zip -q -r -X "$out/$name.zip" .)
(cd "$out" && sha256sum "$name.zip" > "$name.zip.sha256")
echo "$out/$name.zip"
cat "$out/$name.zip.sha256"
