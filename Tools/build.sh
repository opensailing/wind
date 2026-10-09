#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
if ! python3 Tools/runtime_lane.py --check-inherited; then
    exec python3 Tools/runtime_lane.py --label 'Studio build' -- /bin/bash "$PWD/Tools/build.sh" "$@"
fi
ENGINE_PATH="${UE_ENGINE_PATH:-/Users/Shared/Epic Games/UE_5.8}"
mkdir -p tmp/debug
python3 Tools/import_airfoil_sample.py
python3 Tools/import_airfoil_sample.py Content/Samples/MeshGraphNets_Airfoil_test010
"$ENGINE_PATH/Engine/Build/BatchFiles/Mac/Build.sh" LBMStudioEditor Mac Development -project="$PWD/LBMStudio.uproject" -NoHotReload > tmp/debug/editor-build.log 2>&1
cat tmp/debug/editor-build.log
rg -q 'Result: Succeeded' tmp/debug/editor-build.log
"$ENGINE_PATH/Engine/Binaries/Mac/UnrealEditor-Cmd" "$PWD/LBMStudio.uproject" -LLM -run=pythonscript -script="$PWD/Tools/create_materials.py" -unattended -nullrhi -nosplash -stdout -FullStdOutLogOutput > tmp/debug/materials.log 2>&1
rg -q 'STUDIO_MATERIAL_SAVED /Game/Studio/M_FlowScalar' tmp/debug/materials.log
"$ENGINE_PATH/Engine/Binaries/Mac/UnrealEditor-Cmd" "$PWD/LBMStudio.uproject" -LLM -run=pythonscript -script="$PWD/Tools/create_body_material.py" -unattended -nullrhi -nosplash -stdout -FullStdOutLogOutput > tmp/debug/body-material.log 2>&1
rg -q 'STUDIO_BODY_MATERIAL_SAVED /Game/Studio/M_FlowBody' tmp/debug/body-material.log
"$ENGINE_PATH/Engine/Binaries/Mac/UnrealEditor-Cmd" "$PWD/LBMStudio.uproject" -LLM -run=pythonscript -script="$PWD/Tools/create_volume_material.py" -unattended -nullrhi -nosplash -stdout -FullStdOutLogOutput > tmp/debug/volume-material.log 2>&1
rg -q 'STUDIO_VOLUME_MATERIAL_SAVED /Game/Studio/M_FlowVolume' tmp/debug/volume-material.log
"$ENGINE_PATH/Engine/Binaries/Mac/UnrealEditor-Cmd" "$PWD/LBMStudio.uproject" -LLM -run=pythonscript -script="$PWD/Tools/create_mesh_material.py" -unattended -nullrhi -nosplash -stdout -FullStdOutLogOutput > tmp/debug/mesh-material.log 2>&1
rg -q 'STUDIO_MESH_MATERIAL_SAVED /Game/Studio/M_MeshEdges' tmp/debug/mesh-material.log
"$ENGINE_PATH/Engine/Binaries/Mac/UnrealEditor-Cmd" "$PWD/LBMStudio.uproject" -LLM -run=pythonscript -script="$PWD/Tools/create_focused_surface_material.py" -unattended -nullrhi -nosplash -stdout -FullStdOutLogOutput > tmp/debug/focused-surface-material.log 2>&1
rg -q 'STUDIO_MATERIAL_SAVED /Game/Studio/M_FlowScalarFocus' tmp/debug/focused-surface-material.log
echo 'Build and runtime materials ready.'
