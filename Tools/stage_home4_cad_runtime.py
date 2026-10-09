#!/usr/bin/env python3
"""Stage a relocatable, replaceable FreeCAD headless dependency closure on macOS.

Only Python standard library, native extension modules and their Mach-O library
closure are copied. No FreeCAD GUI program/workbenches, CAD examples or user data.
An existing generated destination is replaced transactionally with --refresh. Generated provenance includes every copied SHA.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import urllib.request
import tempfile
import uuid
import zipfile


def dependencies(path: Path) -> list[str]:
    output = subprocess.run(["otool", "-L", str(path)], text=True, capture_output=True, check=True).stdout
    return [line.strip().split(" (", 1)[0] for line in output.splitlines()[1:]]


PIN = Path(__file__).parent / "ThirdParty/home4-cad-source.json"
CACHE = Path(__file__).resolve().parents[1] / "tmp/home4-cad-packages"


def package_licenses(package: dict, destination: Path) -> None:
    """Retain upstream copyright/license texts from the SHA-pinned conda archive."""
    if package["name"] == "freecad":
        return  # Its archive is local-only; the pinned upstream source license is copied below.
    CACHE.mkdir(parents=True, exist_ok=True)
    url = package["url"]
    if not url.startswith("https://conda.anaconda.org/conda-forge/osx-arm64/"):
        raise ValueError("Unsupported dependency source URL")
    archive = CACHE / url.rsplit("/", 1)[1]
    expected = package["sha256"]
    if not expected or len(expected) != 64:
        raise ValueError("Dependency archive requires its SHA256")
    if not archive.is_file() or hashlib.sha256(archive.read_bytes()).hexdigest() != expected:
        with tempfile.NamedTemporaryFile(dir=CACHE, delete=False) as stream:
            pending = Path(stream.name)
            try:
                digest = hashlib.sha256()
                with urllib.request.urlopen(url, timeout=45) as response:
                    count = 0
                    while chunk := response.read(1024 * 1024):
                        count += len(chunk)
                        if count > 256 * 1024 * 1024:
                            raise ValueError("Dependency archive exceeds the staging bound")
                        digest.update(chunk); stream.write(chunk)
                if digest.hexdigest() != expected:
                    raise ValueError("Dependency archive differs from its pinned SHA256")
                stream.flush(); os.fsync(stream.fileno()); os.replace(pending, archive)
            finally:
                pending.unlink(missing_ok=True)
    output = destination / package["name"]
    output.mkdir()
    with zipfile.ZipFile(archive) as package_zip:
        names = [name for name in package_zip.namelist() if name.startswith("info-") and name.endswith(".tar.zst")]
        if len(names) != 1 or package_zip.getinfo(names[0]).file_size > 8 * 1024 * 1024:
            raise ValueError("Invalid bounded dependency metadata archive")
        compressed = package_zip.read(names[0])
    listed = subprocess.run(["/usr/bin/tar", "-tf", "-"], input=compressed, capture_output=True, check=True).stdout.decode().splitlines()
    recipe_names = [name for name in listed if name.startswith("info/recipe/") and not name.endswith("/") and "/parent/" not in name]
    recipes = output / "source-build-recipe"
    recipes.mkdir()
    for name in recipe_names:
        payload = subprocess.run(["/usr/bin/tar", "-xOf", "-", name], input=compressed, capture_output=True, check=True).stdout
        if len(payload) > 2 * 1024 * 1024:
            raise ValueError("Source recipe exceeds the staging bound")
        recipes.joinpath(name.removeprefix("info/recipe/").replace("/", "_")).write_bytes(payload)
    texts = [name for name in listed if name.startswith("info/licenses/") and not name.endswith("/")]
    if not texts:
        texts_root = Path(__file__).parent / "ThirdParty/Home4CADLicenses"
        record = json.loads(texts_root.joinpath("manifest.json").read_text()).get(package["name"])
        original = texts_root / (package["name"] + ".txt")
        if not record or not original.is_file() or hashlib.sha256(original.read_bytes()).hexdigest() != record["sha256"]:
            raise ValueError("Verified upstream license text is missing: " + package["name"])
        shutil.copy2(original, output / "LICENSE.txt")
        return
    for name in texts:
        # Extract only each explicitly named regular text, never archive paths.
        payload = subprocess.run(["/usr/bin/tar", "-xOf", "-", name], input=compressed, capture_output=True, check=True).stdout
        if len(payload) > 2 * 1024 * 1024:
            raise ValueError("License text exceeds the staging bound")
        output.joinpath(name.removeprefix("info/licenses/").replace("/", "_")).write_bytes(payload)


def _stage(resources: Path, destination: Path) -> dict:
    resources = resources.resolve()
    if destination.exists():
        raise ValueError("Runtime destination already exists; never replace a published dependency")
    if not (resources / "bin/python").is_file():
        raise ValueError("Expected the verified FreeCAD Resources directory")
    pin = json.loads(PIN.read_text())
    installed = {json.loads(p.read_text())["name"]: json.loads(p.read_text()) for p in (resources / "conda-meta").glob("*.json")}
    for package in pin["packages"]:
        actual = installed.get(package["name"], {})
        if any(actual.get(key) != package[key] for key in ["version", "build", "sha256"]):
            raise ValueError("Installed CAD dependency differs from the pinned source manifest: " + package["name"])
    for relative, expected in pin["native_files"].items():
        path = resources / relative
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError("Installed CAD binary differs from the pinned original SHA256: " + relative)
    destination.mkdir(parents=True)
    closure: dict[str, Path] = {}
    pending = [resources / "bin/python", resources / "lib/FreeCAD.so", resources / "lib/Part.so"]
    stdlib = resources / "lib/python3.11"
    for module in (stdlib / "lib-dynload").glob("*.so"):
        if module.stem.split(".")[0] not in {"readline", "_tkinter"}:
            pending.append(module)
    while pending:
        path = pending.pop()
        key = str(path.relative_to(resources))
        if key in closure:
            continue
        closure[key] = path
        for dependency in dependencies(path):
            if dependency.startswith(("/usr/lib/", "/System/Library/")):
                continue
            if dependency.startswith("@rpath/"):
                candidate = resources / "lib" / dependency.removeprefix("@rpath/")
            elif dependency.startswith("@loader_path/"):
                candidate = path.parent / dependency.removeprefix("@loader_path/")
            elif dependency.startswith(str(resources)):
                candidate = Path(dependency)
            else:
                raise ValueError(f"Unresolved external runtime dependency: {dependency}")
            if not candidate.exists():
                raise ValueError(f"Missing runtime dependency: {candidate}")
            pending.append(candidate)
    shutil.copytree(stdlib, destination / "lib/python3.11", ignore=shutil.ignore_patterns("site-packages", "__pycache__", "test", "tests", "idlelib", "tkinter", "turtledemo", "ensurepip", "readline*.so", "_tkinter*.so"))
    for relative, original in closure.items():
        output = destination / relative
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(original, output, follow_symlinks=True)
    # Binary distributions are separate and replaceable. Preserve installed
    # upstream package metadata/licenses, and include FreeCAD's actual license.
    license_dir = destination / "licenses"
    license_dir.mkdir()
    shutil.copytree(resources / "conda-meta", license_dir / "upstream-package-metadata")
    source = "https://raw.githubusercontent.com/FreeCAD/FreeCAD/34a9716668b1ddeb55b914f1c5be644826bdbbbf/src/Doc/LICENSE.html"
    license_dir.joinpath("FreeCAD-LICENSE.html").write_bytes(urllib.request.urlopen(source, timeout=30).read())
    if (resources / "share/licenses").exists():
        shutil.copytree(resources / "share/licenses", license_dir / "upstream-license-texts")
    if (resources / "share/icu/75.1/LICENSE").exists():
        shutil.copy2(resources / "share/icu/75.1/LICENSE", license_dir / "ICU-LICENSE")
    for name in ["pcre2", "qhull", "gl2ps", "opencascade", "libjpeg-turbo"]:
        directory = resources / "share/doc" / name
        if directory.exists():
            shutil.copytree(directory, license_dir / name)
    upstream = license_dir / "dependency-license-texts"
    upstream.mkdir()
    for package in pin["packages"]:
        package_licenses(package, upstream)
    shutil.copy2(PIN, license_dir / "source-manifest.json")
    shutil.copytree(Path(__file__).parent / "ThirdParty/Home4CADLicenses", license_dir / "pinned-upstream-license-texts")
    shutil.copy2(Path(__file__).parent / "home4_cad_prepare.py", destination / "home4_cad_prepare.py")
    license_dir.joinpath("NOTICE.md").write_text(
        "# HOME4 CAD preprocessing dependency\n\n"
        "This separate, replaceable runtime contains unmodified FreeCAD, OpenCASCADE, CPython and their shared-library dependencies. "
        "FreeCAD is LGPL-2.1-or-later; see FreeCAD-LICENSE.html. Source revision: https://github.com/FreeCAD/FreeCAD/tree/34a9716668b1ddeb55b914f1c5be644826bdbbbf. "
        "Dependency versions, upstream source/download URLs and license identifiers are retained under upstream-package-metadata. "
        "Users may substitute an ABI-compatible FreeCAD Python/kernel through the native authoring configuration. "
        "Copyright notices and license texts are under dependency-license-texts and pinned-upstream-license-texts. "
        "The exact upstream build recipes, patch sets and source archive URLs are preserved alongside them. "
        "FreeImage is redistributed under its FreeImage Public License, not the alternative GPL; see its full license text. "
        "The native app invokes these unmodified shared libraries in a separate replaceable process. "
        "Qt libraries are LGPL-3.0-only; LGPL/GPL texts and replacement configuration are supplied. "
        "No HOME4 solver or numerical validation is included.\n", encoding="utf-8")
    # Verify relocatability with no inherited PYTHONPATH or GUI environment.
    code = "import sys;from pathlib import Path;sys.path.insert(0,str(Path(sys.executable).resolve().parent.parent/'lib'));import FreeCAD,Part;assert '.'.join(FreeCAD.Version()[:3])=='" + pin["freecad_version"] + "';assert abs(Part.makeBox(2,3,4).Volume-24)<1e-12;print(FreeCAD.__file__)"
    verified = subprocess.run([str(destination / "bin/python"), "-I", "-B", "-c", code], env={"PATH":"/usr/bin:/bin", "HOME":str(destination)}, text=True, capture_output=True, check=True, timeout=45)
    if str(destination.resolve()) not in verified.stdout:
        raise ValueError("CAD runtime resolved an external FreeCAD installation")
    files = {}
    for path in sorted(destination.rglob("*")):
        if path.is_file():
            files[str(path.relative_to(destination))] = {"bytes": path.stat().st_size, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    manifest = {"license_manifest_sha256": hashlib.sha256(Path(__file__).parent.joinpath("ThirdParty/Home4CADLicenses/manifest.json").read_bytes()).hexdigest(), "version": 1, "source_resources": str(resources), "files": files,
                "bytes": sum(entry["bytes"] for entry in files.values()), "binary_closure_count": len(closure), "freecad_version": pin["freecad_version"], "source_manifest_sha256": hashlib.sha256(PIN.read_bytes()).hexdigest(), "isolated_kernel_volume_check": 24}
    destination.joinpath("runtime-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def stage(resources: Path, destination: Path, refresh: bool = False) -> dict:
    if destination.is_symlink():
        raise ValueError("Runtime destination must not be a symlink")
    destination = destination.resolve()
    expected_pin = hashlib.sha256(PIN.read_bytes()).hexdigest()
    helper_sha = hashlib.sha256(Path(__file__).parent.joinpath("home4_cad_prepare.py").read_bytes()).hexdigest()
    if destination.exists():
        manifest_path = destination / "runtime-manifest.json"
        if not manifest_path.is_file():
            raise ValueError("Existing directory is not an owned HOME4 CAD runtime")
        original = json.loads(manifest_path.read_text())
        files = original.get("files", {})
        if not isinstance(files, dict) or not 1 <= len(files) <= 20000:
            raise ValueError("Existing runtime inventory is invalid")
        for relative in files:
            name = PurePosixPath(relative)
            path = destination / relative
            if name.is_absolute() or '..' in name.parts or path.is_symlink() or not path.resolve().is_relative_to(destination):
                raise ValueError("Existing runtime inventory is not a regular internal path")
        source_licenses = Path(__file__).parent / "ThirdParty/Home4CADLicenses/manifest.json"
        expected_licenses = hashlib.sha256(source_licenses.read_bytes()).hexdigest()
        same = original.get("source_manifest_sha256") == expected_pin and original.get("license_manifest_sha256") == expected_licenses and original.get("files", {}).get("home4_cad_prepare.py", {}).get("sha256") == helper_sha
        actual = {str(path.relative_to(destination)) for path in destination.rglob('*') if path.is_file() or path.is_symlink()}
        same = same and actual == set(files) | {"runtime-manifest.json"}
        if same and all(path.is_file() and path.stat().st_size == entry["bytes"] and hashlib.sha256(path.read_bytes()).hexdigest() == entry["sha256"] for relative, entry in original["files"].items() for path in [destination / relative]):
            return original
        if not refresh:
            raise ValueError("Staged runtime changed; use --refresh to transactionally restage generated artifacts")
    destination.parent.mkdir(parents=True, exist_ok=True)
    pending = destination.with_name(destination.name + ".pending-" + uuid.uuid4().hex)
    previous = destination.with_name(destination.name + ".previous-" + uuid.uuid4().hex)
    try:
        result = _stage(resources, pending)
        if destination.exists():
            os.replace(destination, previous)
        os.replace(pending, destination)
        shutil.rmtree(previous, ignore_errors=True)
        return result
    except BaseException:
        shutil.rmtree(pending, ignore_errors=True)
        if previous.exists() and not destination.exists():
            os.replace(previous, destination)
        raise


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--resources", type=Path, default=Path(os.environ.get("HOME4_FREECAD_RESOURCES", "/Applications/FreeCAD.app/Contents/Resources")))
    parser.add_argument("--refresh", action="store_true", help="Replace only a recognized generated runtime, transactionally")
    args = parser.parse_args()
    result = stage(args.resources, args.destination, args.refresh)
    print(json.dumps({key: value for key, value in result.items() if key != "files"}))
