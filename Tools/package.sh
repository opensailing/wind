#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
if ! python3 Tools/runtime_lane.py --check-inherited; then
    exec python3 Tools/runtime_lane.py --label 'Studio package' -- /bin/bash "$PWD/Tools/package.sh" "$@"
fi
ENGINE_PATH="${UE_ENGINE_PATH:-/Users/Shared/Epic Games/UE_5.8}"
mkdir -p tmp/debug
python3 Tools/import_airfoil_sample.py
python3 Tools/import_airfoil_sample.py Content/Samples/MeshGraphNets_Airfoil_test010
python3 Tools/import_nalu_history.py
python3 Tools/stage_home4_cad_runtime.py Content/ThirdParty/Home4CAD --refresh > tmp/debug/home4-cad-runtime.json
# Build code first, then finalize the app outside UBA. Xcode 27 cannot inherit
# UBA's redirected descriptors when launching scheme pre-actions.
UE_BUILD_FROM_XCODE=1 "$ENGINE_PATH/Engine/Build/BatchFiles/Mac/Build.sh" LBMStudio Mac Development -project="$PWD/LBMStudio.uproject" -NoUBA > tmp/debug/game-build.log 2>&1
rg -q 'Result: Succeeded' tmp/debug/game-build.log
"$ENGINE_PATH/Engine/Binaries/ThirdParty/DotNet/10.0/mac-arm64/dotnet" "$ENGINE_PATH/Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.dll" -Mode=ApplePostBuildSync -Input="$PWD/Intermediate/Build/Mac/arm64/LBMStudio/Development/PostBuildSync_LBMStudio.json" -XmlConfigCache="$PWD/Intermediate/Build/XmlConfigCache.bin" -remoteini="$PWD" </dev/null > tmp/debug/postbuild.log 2>&1
"$ENGINE_PATH/Engine/Build/BatchFiles/RunUAT.sh" BuildCookRun -project="$PWD/LBMStudio.uproject" -platform=Mac -clientconfig=Development -skipbuild -nocompileeditor -cook -stage -pak -package -archive -archivedirectory="$PWD/Packaged" -unattended > tmp/debug/package.log 2>&1
rg -q 'BUILD SUCCESSFUL' tmp/debug/package.log
# Validate the signed artifact, because Xcode may merge or replace source entitlements.
python3 Tools/verify_macos_package.py "$PWD/Packaged/Mac/LBMStudio.app" > tmp/debug/package-entitlements.json
echo "Packaged application: $PWD/Packaged/Mac/LBMStudio.app"
echo "Local Development package; distribution signing/notarization has not been verified."
echo "After Developer ID signing, notarization and stapling, check it with:"
echo "python3 Tools/verify_macos_package.py Packaged/Mac/LBMStudio.app --distribution"
