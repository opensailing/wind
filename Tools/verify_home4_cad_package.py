#!/usr/bin/env python3
"""Verify real STEP/IGES preparation by the shipped runtime after app relocation."""
from __future__ import annotations
import argparse
from contextlib import redirect_stdout
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import plistlib
import shutil
import subprocess
import sys
import tempfile
import uuid

from run_studio import _run_owned
from runtime_lane import lease
from validate import evaluate


NATIVE_TEST = 'Studio.Home4.Authoring.PackagedCADRoundTrip'


def verify_native_import(app: Path, output: Path) -> dict:
    """Exercise the signed sandboxed app's real CreateProc/import path."""
    project = Path(__file__).resolve().parents[1]
    with (app / 'Contents/Info.plist').open('rb') as stream:
        bundle = plistlib.load(stream)['CFBundleIdentifier']
    saved = Path.home() / 'Library/Containers' / bundle / 'Data/Library/Application Support/Epic/LBMStudio/Saved'
    report = saved / 'Automation' / ('home4-cad-' + uuid.uuid4().hex)
    report.mkdir(parents=True)
    command = [str(app / 'Contents/MacOS/LBMStudio'), '-unattended', '-nullrhi',
               '-RenderOffscreen', '-nosplash', '-nosound', '-stdout', '-FullStdOutLogOutput',
               '-notraceserver', '-NoZenAutoLaunch', '-ddc=InstalledNoZenLocalFallback',
               f'-ExecCmds=Automation RunTests {NATIVE_TEST}', '-TestExit=Automation Test Queue Empty',
               f'-ReportExportPath={report}']
    try:
        with lease(project, 'Relocated HOME4 native CAD import'), redirect_stdout(sys.stderr):
            code = _run_owned(command, project, timeout=180,
                              log_path=output / 'native.log', report_path=output / 'native-process.json')
        lifecycle = json.loads((output / 'native-process.json').read_text())
        shutil.copytree(report, output / 'native-automation')
        result = evaluate(json.loads((report / 'index.json').read_text(encoding='utf-8-sig')), [NATIVE_TEST])
        if code or lifecycle.get('cleanup_required') or lifecycle.get('owned_processes_after'):
            result['errors'].append('Packaged native CAD process failed or required cleanup')
        result['passed'] = not result['errors']
        result['application_sha256'] = sha(app / 'Contents/MacOS/LBMStudio')
        result['external_llm_flag_supplied'] = False
        result['scope'] = 'Relocated sandboxed application, bundled CAD child process, native SDF preparation; NullRHI'
        return result
    finally:
        shutil.rmtree(report)


def sha(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def runtime_in(app: Path) -> Path:
    matches = list(app.joinpath('Contents').rglob('Home4CAD/runtime-manifest.json'))
    if len(matches) != 1:
        raise ValueError('The app must contain exactly one HOME4 CAD runtime manifest')
    return matches[0].parent


def verify_files(root: Path) -> dict:
    path = root / 'runtime-manifest.json'
    if path.stat().st_size > 2 * 1024 * 1024:
        raise ValueError('Runtime manifest exceeds its bounded metadata size')
    manifest = json.loads(path.read_text())
    files = manifest.get('files', {})
    if not isinstance(files, dict) or not 1 <= len(files) <= 20000:
        raise ValueError('Invalid runtime inventory')
    for relative, entry in files.items():
        name = PurePosixPath(relative)
        if name.is_absolute() or '..' in name.parts or not name.parts:
            raise ValueError('Unsafe runtime inventory path')
        candidate = root.joinpath(*name.parts)
        if candidate.is_symlink() or not candidate.is_file() or not candidate.resolve().is_relative_to(root.resolve()):
            raise ValueError('Runtime inventory does not name a regular internal file: ' + relative)
        if candidate.stat().st_size != entry['bytes'] or sha(candidate) != entry['sha256']:
            raise ValueError('Shipped runtime differs from its pinned staged bytes: ' + relative)
    actual = {str(path.relative_to(root)) for path in root.rglob('*') if path.is_file() or path.is_symlink()}
    if actual != set(files) | {'runtime-manifest.json'}:
        difference = sorted(actual.symmetric_difference(set(files) | {'runtime-manifest.json'}))
        raise ValueError('Runtime contains missing or unlisted files: ' + ', '.join(difference[:10]))
    if not any(name.startswith('licenses/dependency-license-texts/') for name in files):
        raise ValueError('Dependency license texts were not shipped')
    for package in ('libglib', 'pcre2'):
        records = [root / name for name in files if name.startswith('licenses/dependency-license-texts/' + package + '/') and not 'source-build-recipe/' in name]
        if not records or not any(path.stat().st_size > 1000 and b'Copyright' in path.read_bytes() for path in records):
            raise ValueError('Actual referenced license text was not shipped: ' + package)
    return manifest


def verify(app: Path, output: Path) -> dict:
    app = app.resolve()
    if not app.is_dir() or output.exists():
        raise ValueError('Supply an existing application and an absent verification output directory')
    output.mkdir(parents=True)
    # Copy the complete app, then use only its relocated interpreter/helper. This
    # exercises UE's actual NonUFS path and cannot resolve the original CAD app.
    with tempfile.TemporaryDirectory(prefix='home4-relocated-package-') as work:
        work = Path(work)
        relocated = work / app.name
        subprocess.run(['/usr/bin/ditto', str(app), str(relocated)], check=True, timeout=180)
        root = runtime_in(relocated)
        manifest = verify_files(root)
        interpreter = root / 'bin/python'
        library = root / 'lib'
        env = {'PATH': '/usr/bin:/bin', 'HOME': str(work / 'isolated-home'), 'TMPDIR': str(work)}
        Path(env['HOME']).mkdir()
        create = "import sys,json;from pathlib import Path;assert sys.dont_write_bytecode;sys.path.insert(0,sys.argv[1]);import FreeCAD,Part;assert Path(FreeCAD.__file__).resolve().is_relative_to(Path(sys.argv[1]).resolve());shape=Part.makeBox(2,3,4);shape.exportStep(sys.argv[2]);shape.exportIges(sys.argv[3]);print(json.dumps({'kernel':'.'.join(FreeCAD.Version()[:3]),'module':str(FreeCAD.__file__),'volume':shape.Volume}))"
        step, iges = output / 'original.step', output / 'original.iges'
        generated = subprocess.run([str(interpreter), '-I', '-B', '-c', create, str(library), str(step), str(iges)], env=env, capture_output=True, text=True, timeout=45, check=True)
        kernel = next(json.loads(line) for line in generated.stdout.splitlines() if line.startswith('{"kernel"'))
        outputs = []
        for source in (step, iges):
            stl = output / (source.stem + source.suffix + '.stl')
            command = [str(interpreter), '-I', '-B', str(root / 'home4_cad_prepare.py'), '--library', str(library), '--input', str(source), '--output', str(stl), '--tolerance', '0.01', '--expected-sha256', sha(source)]
            subprocess.run(command, env=env, capture_output=True, text=True, timeout=45, check=True)
            result = json.loads(Path(str(stl) + '.json').read_text())
            if result.get('source_sha256') != sha(source) or result.get('output_sha256') != sha(stl) or abs(result.get('volume_source_units3', 0) - 24) > 1e-9:
                raise ValueError('Relocated CAD output failed original-byte/closed-volume integrity checks')
            outputs.append(result)
        # Report each nested Mach-O seal separately; the parent package checker
        # also verifies the complete app. This tool never changes signatures.
        signatures = []
        for relative in manifest['files']:
            path = root / relative
            with path.open('rb') as stream:
                magic = stream.read(4)
            if magic not in (b'\xcf\xfa\xed\xfe', b'\xfe\xed\xfa\xcf', b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'):
                continue
            result = subprocess.run(['/usr/bin/codesign', '--verify', '--strict', str(path)], capture_output=True, text=True, timeout=15)
            signatures.append({'path': relative, 'verified': result.returncode == 0, 'details': result.stderr.strip()})
        nested_passed = bool(signatures) and all(record['verified'] for record in signatures)
        native = verify_native_import(relocated, output)
        # Importing Python modules must not add bytecode or alter signed runtime
        # contents. Verify again after the application's real child has exited.
        verify_files(root)
        seal = subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(relocated)],
                              capture_output=True, text=True, timeout=60)
        after = {'passed': seal.returncode == 0, 'details': seal.stderr.strip()}
        result = {'passed': nested_passed and native['passed'] and after['passed'], 'application': str(app), 'relocated_application': str(relocated), 'runtime_relative_path': str(root.relative_to(relocated)), 'isolated_environment': sorted(env), 'external_cad_installation_used': False, 'kernel': kernel['kernel'], 'closed_volume_source_units': kernel['volume'], 'source_manifest_sha256': manifest['source_manifest_sha256'], 'outputs': outputs, 'nested_signatures': signatures, 'nested_signatures_passed': nested_passed, 'native_import': native, 'post_import_application_seal': after, 'scope': 'relocated local-package CAD functional verification; distribution signing remains a separate gate'}
        output.joinpath('verification.json').write_text(json.dumps(result, indent=2) + '\n')
        return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('application', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        result = verify(args.application, args.output)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(json.dumps({'passed': False, 'error': str(error)}, indent=2))
        return 1
    print(json.dumps(result, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
