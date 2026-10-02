#!/usr/bin/env python3
"""Build and validate without windows, screenshots, native input or a GPU.

Runs the real Unreal model and virtual Slate tests under NullRHI. A checked-in
catalog makes missing/skipped tests fail. JSON summarizes failures and widget
layout artifacts; native-dialog and GPU acceptance remain separate workflows.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess

from run_studio import _run_owned
from runtime_lane import serialized
from test_stability import inventory, relevant


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def evaluate(report, expected):
    """Fail closed: exact names and terminal states, not an exit code or count."""
    tests = report.get('tests', [])
    names = [t.get('fullTestPath', '') for t in tests]
    errors = []
    if len(set(names)) != len(names):
        errors.append('Duplicate test identities in report')
    missing, extra = sorted(set(expected)-set(names)), sorted(set(names)-set(expected))
    if missing:
        errors.append('Missing expected tests: '+', '.join(missing))
    if extra:
        errors.append('Unexpected tests: update the reviewed headless catalog: '+', '.join(extra))
    failures, warnings = [], []
    for test in tests:
        events = [entry.get('event', {}) for entry in test.get('entries', [])]
        bad = [e.get('message', '') for e in events if e.get('type') == 'Error']
        warn = [e.get('message', '') for e in events if e.get('type') == 'Warning']
        if test.get('state') != 'Success' or bad:
            failures.append({'test': test.get('fullTestPath'), 'state': test.get('state'), 'errors': bad})
        if warn:
            warnings.append({'test': test.get('fullTestPath'), 'messages': warn})
    if failures:
        errors.append('One or more tests failed or did not complete')
    if (report.get('failed', 0) or report.get('notRun', 0) or report.get('inProcess', 0)
            or report.get('succeeded', 0)+report.get('succeededWithWarnings', 0) != len(expected)):
        errors.append('Automation totals disagree with the required completed tests')
    return {'passed': not errors, 'errors': errors, 'tests': len(tests), 'failures': failures, 'warnings': warnings}


@serialized('Headless validation')
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--suite', default='Studio.', help='Catalog prefix, e.g. Studio.HeadlessUI.')
    parser.add_argument('--no-build', action='store_true', help='Explicitly validate the existing editor module')
    parser.add_argument('--timeout', type=int, default=300, help='Test process deadline in seconds')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    catalog = json.loads((root/'Tools/headless-tests.json').read_text())['tests']
    if any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._' for c in args.suite):
        parser.error('Use a headless test prefix with letters, digits, dots and underscores.')
    expected = [name for name in catalog if name.startswith(args.suite)]
    if not expected or args.timeout < 10:
        parser.error('Choose a prefix from Tools/headless-tests.json and a timeout of at least 10 seconds.')
    before = relevant(inventory())
    if any(Path(p['command']).name not in {'CrashReportClient', 'CrashReportClientEditor'} for p in before.values()):
        parser.error('Unreal work is already running; existing processes were left untouched.')
    engine = Path(os.environ.get('UE_ENGINE_PATH', '/Users/Shared/Epic Games/UE_5.8'))
    editor = engine/'Engine/Binaries/Mac/UnrealEditor-Cmd'
    if not editor.is_file():
        parser.error('Set UE_ENGINE_PATH to an installed Unreal Engine containing UnrealEditor-Cmd.')
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    output = root/'tmp/debug'/f'headless-{stamp}'
    output.mkdir(parents=True)
    source_files = sorted(p for folder in ('Source', 'Config') for p in (root/folder).rglob('*') if p.is_file())
    sources = {str(p.relative_to(root)): digest(p) for p in [root/'LBMStudio.uproject', *source_files]}
    summary = {'passed': False, 'mode': 'headless-nullrhi', 'suite': args.suite, 'build_requested': not args.no_build,
               'report': str(output), 'expected_tests': len(expected), 'errors': [], 'sources': sources,
               'coverage': ['C++ model behavior', 'Registered virtual Slate widget workflows and structural layout'],
               'excluded': ['GPU rendering and Metal stability', 'Native file dialogs and physical OS input', 'Visual design review'],
               'screenshots': 0, 'processes_before': before}
    try:
        if not args.no_build:
            command = [str(engine/'Engine/Build/BatchFiles/Mac/Build.sh'), 'LBMStudioEditor', 'Mac', 'Development',
                       f'-project={root/"LBMStudio.uproject"}', '-NoHotReload', '-DisableAdaptiveUnity']
            code = _run_owned(command, root, timeout=1200, log_path=output/'build.log', report_path=output/'build-process.json')
            if code:
                raise RuntimeError(f'Build failed ({code}); see build.log')
        module = root/'Binaries/Mac/libUnrealEditor-LBMStudio.dylib'
        summary['module_sha256'] = digest(module)
        summary['headless_catalog_sha256'] = digest(root/'Tools/headless-tests.json')
        command = [str(editor), str(root/'LBMStudio.uproject'), '-LLM', '-unattended', '-nullrhi', '-RenderOffscreen',
                   '-nosplash', '-nosound', '-stdout', '-FullStdOutLogOutput', '-notraceserver', '-NoZenAutoLaunch', '-ddc=InstalledNoZenLocalFallback',
                   f'-ExecCmds=Automation RunTests {args.suite}', '-TestExit=Automation Test Queue Empty',
                   f'-ReportExportPath={output/"automation"}', f'-StudioHeadlessOutput={output/"widgets"}']
        code = _run_owned(command, root, timeout=args.timeout, log_path=output/'tests.log', report_path=output/'process.json')
        lifecycle = json.loads((output/'process.json').read_text())
        summary['processes_after'] = lifecycle['processes_after']
        summary['owned_processes_after'] = lifecycle['owned_processes_after']
        summary['elapsed_seconds'] = lifecycle.get('elapsed_seconds')
        if code or lifecycle.get('cleanup_required'):
            summary['errors'].append(f'Headless process failed or needed cleanup ({code}); see process.json')
        report = json.loads((output/'automation/index.json').read_text(encoding='utf-8-sig'))
        result = evaluate(report, expected)
        summary.update({k: result[k] for k in ('tests', 'failures', 'warnings')})
        summary['errors'] += result['errors']
        if 'rhiname="Null"' not in (output/'tests.log').read_text(errors='replace'):
            summary['errors'].append('Engine did not confirm NullRHI')
        if any(digest(root/path) != sha for path, sha in sources.items()):
            summary['errors'].append('Source changed during validation')
        summary['widget_reports'] = [str(p.relative_to(output)) for p in sorted((output/'widgets').glob('*.json'))]
        if any(name.startswith('Studio.HeadlessUI.') for name in expected) and not summary['widget_reports']:
            summary['errors'].append('Virtual Slate tests produced no structural reports')
        for path in summary['widget_reports']:
            widget = json.loads((output/path).read_text(encoding='utf-8'))
            if not widget.get('passed') or not widget.get('controls'):
                summary['errors'].append(f'Invalid structural widget report: {path}')
        summary['passed'] = not summary['errors']
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as error:
        summary['errors'].append(str(error))
    (output/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    print(json.dumps({k: summary[k] for k in ('passed', 'mode', 'suite', 'expected_tests', 'errors', 'report')}, indent=2), flush=True)
    return 0 if summary['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
