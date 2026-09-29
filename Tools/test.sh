#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
if ! python3 Tools/runtime_lane.py --check-inherited; then
    exec python3 Tools/runtime_lane.py --label 'Studio model tests' -- /bin/bash "$PWD/Tools/test.sh" "$@"
fi
ENGINE_PATH="${UE_ENGINE_PATH:-/Users/Shared/Epic Games/UE_5.8}"
mkdir -p tmp/debug
"$ENGINE_PATH/Engine/Binaries/Mac/UnrealEditor-Cmd" "$PWD/LBMStudio.uproject" -LLM -unattended -nullrhi -nosplash -stdout -FullStdOutLogOutput -ExecCmds='Automation RunTests Studio.' -TestExit='Automation Test Queue Empty' -ReportExportPath="$PWD/tmp/debug/automation" > tmp/debug/tests.log 2>&1
python3 - <<'PY'
import json
from pathlib import Path
report=json.loads(Path('tmp/debug/automation/index.json').read_text(encoding='utf-8-sig'))
passed=report['succeeded']+report.get('succeededWithWarnings',0)
if passed < 259 or report['failed'] or report.get('notRun', 0):
    for test in report.get('tests', []):
        if test.get('state') not in ('Success', 'NotRun'):
            print(test.get('fullTestPath'), test.get('state'))
            for entry in test.get('entries', []):
                event=entry.get('event', {})
                if event.get('type') in ('Error', 'Warning'):
                    print(event.get('type'), event.get('message'))
    raise SystemExit(f"Studio tests incomplete: {passed} passed, {report['failed']} failed, {report.get('notRun', 0)} not run")
print(f"{passed} Studio automation tests passed ({report.get('succeededWithWarnings',0)} with warnings)")
PY
