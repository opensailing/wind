#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
if ! python3 Tools/runtime_lane.py --check-inherited; then
    exec python3 Tools/runtime_lane.py --label 'Studio render tests' -- /bin/bash "$PWD/Tools/test-render.sh" "$@"
fi
APP="$PWD/Packaged/Mac/LBMStudio.app/Contents/MacOS/LBMStudio"
DURATION="${1:-45}"
[[ "$DURATION" =~ ^[0-9]+$ ]] || { echo 'Duration must be whole seconds.' >&2; exit 1; }
[[ -x "$APP" ]] || { echo 'Run Tools/package.sh first.' >&2; exit 1; }
if pgrep -x LBMStudio >/dev/null; then
    echo 'Close LBMStudio before running its GPU test.' >&2
    exit 1
fi
mkdir -p tmp/debug/render-automation
# A packaged Mac app can write reports inside its application container.
# Copy the machine-readable result back to the workspace after it exits.
BUNDLE_ID=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$PWD/Packaged/Mac/LBMStudio.app/Contents/Info.plist")
REPORT_PATH="$HOME/Library/Containers/$BUNDLE_ID/Data/Library/Application Support/Epic/LBMStudio/Saved/Automation/RenderSoakReport"
mkdir -p "$REPORT_PATH"
rm -f "$REPORT_PATH/index.json"
"$APP" -LLM -windowed -ResX=1320 -ResY=740 -unattended -stdout -FullStdOutLogOutput \
    '-LogCmds=LogTemp Verbose' -StudioAutomation \
    -ExecCmds='Automation RunTests Studio.Rendering.' \
    -TestExit='Automation Test Queue Empty' \
    -StudioRenderSoakSeconds="$DURATION" \
    -ReportExportPath="$REPORT_PATH" > tmp/debug/render-soak.log 2>&1
cp "$REPORT_PATH/index.json" tmp/debug/render-automation/index.json
python3 - <<'PY'
import json
from pathlib import Path
report=json.loads(Path('tmp/debug/render-automation/index.json').read_text(encoding='utf-8-sig'))
assert report['succeeded'] == 6 and report['failed'] == 0, report
print('GPU rendering soak passed')
PY
