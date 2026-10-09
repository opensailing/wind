#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
APP="$PWD/Packaged/Mac/LBMStudio.app/Contents/MacOS/LBMStudio"
[[ -x "$APP" ]] || { echo 'Run Tools/package.sh first.' >&2; exit 1; }
if pgrep -x LBMStudio >/dev/null; then
    echo 'Close LBMStudio before running its packaged job-control tests.' >&2
    exit 1
fi
mkdir -p tmp/debug/job-automation
BUNDLE_ID=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$PWD/Packaged/Mac/LBMStudio.app/Contents/Info.plist")
REPORT_PATH="$HOME/Library/Containers/$BUNDLE_ID/Data/Library/Application Support/Epic/LBMStudio/Saved/Automation/JobReport"
mkdir -p "$REPORT_PATH"
rm -f "$REPORT_PATH/index.json"
"$APP" -LLM -windowed -ResX=1320 -ResY=740 -unattended -stdout -FullStdOutLogOutput \
    -StudioAutomation -ExecCmds='Automation RunTests Studio.Jobs.' \
    -TestExit='Automation Test Queue Empty' -ReportExportPath="$REPORT_PATH" \
    > tmp/debug/job-packaged-tests.log 2>&1
cp "$REPORT_PATH/index.json" tmp/debug/job-automation/index.json
python3 - <<'PY'
import json
from pathlib import Path
report=json.loads(Path('tmp/debug/job-automation/index.json').read_text(encoding='utf-8-sig'))
assert report['succeeded'] == 7 and report['failed'] == 0 and report['notRun'] == 0, report
print('7 packaged job-control tests passed')
PY
