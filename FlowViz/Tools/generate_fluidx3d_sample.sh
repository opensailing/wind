#!/usr/bin/env bash
# Generate the genuine 3D FluidX3D sphere-wake demo/performance case.
#
# The source checkout is an input only. All patching, building, simulation, and
# export happen in a new --no-local clone pinned to REVISION.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FLOWVIZ_ROOT="$(cd "${HERE}/.." && pwd)"
REPO_ROOT="$(cd "${FLOWVIZ_ROOT}/.." && pwd)"
REVISION="024e48c23256a31346cf458fba76deae4aca7869"
SHORT_REVISION="${REVISION:0:8}"
SETUP_SOURCE="${HERE}/fluidx3d/sphere_wake_setup.cpp"
DEFINES_PATCH="${HERE}/fluidx3d/defines.patch"
RECIPE_README="${HERE}/fluidx3d/README.md"

SOURCE="${FLOWVIZ_FLUIDX3D_SOURCE:-${REPO_ROOT}/../FluidX3D}"
WORK_DIR="${FLOWVIZ_FLUIDX3D_WORK_DIR:-${HOME}/projects/flowviz-fluidx3d-${SHORT_REVISION}}"
OUTPUT="${FLOWVIZ_FLUIDX3D_OUTPUT:-${FLOWVIZ_ROOT}/Samples/FluidX3DSphereWake.cfdviz}"
FORCE=0

usage() {
    cat <<EOF
Usage: Tools/generate_fluidx3d_sample.sh [options]

Options:
  --source PATH      Existing FluidX3D git checkout used only as a clone source
  --work-dir PATH    New isolated generation directory
  --output PATH      CFDViz output directory
  --force            Replace this script's existing work/output/release paths
  -h, --help         Show this help
EOF
}

while [[ "$#" -gt 0 ]]; do
    case "$1" in
        --source)
            [[ "$#" -ge 2 ]] || { echo "--source requires a path" >&2; exit 2; }
            SOURCE="$2"
            shift 2
            ;;
        --work-dir)
            [[ "$#" -ge 2 ]] || { echo "--work-dir requires a path" >&2; exit 2; }
            WORK_DIR="$2"
            shift 2
            ;;
        --output)
            [[ "$#" -ge 2 ]] || { echo "--output requires a path" >&2; exit 2; }
            OUTPUT="$2"
            shift 2
            ;;
        --force)
            FORCE=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

for required in "${SETUP_SOURCE}" "${DEFINES_PATCH}" "${RECIPE_README}"; do
    [[ -f "${required}" ]] || { echo "missing recipe input: ${required}" >&2; exit 1; }
done
git -C "${SOURCE}" rev-parse --git-dir >/dev/null 2>&1 || {
    echo "FluidX3D source is not a git checkout: ${SOURCE}" >&2
    exit 1
}

SOURCE="$(cd "${SOURCE}" && pwd -P)"
WORK_PARENT="$(dirname "${WORK_DIR}")"
mkdir -p "${WORK_PARENT}"
WORK_PARENT="$(cd "${WORK_PARENT}" && pwd -P)"
WORK_DIR="${WORK_PARENT}/$(basename "${WORK_DIR}")"
OUTPUT_PARENT="$(dirname "${OUTPUT}")"
mkdir -p "${OUTPUT_PARENT}"
OUTPUT_PARENT="$(cd "${OUTPUT_PARENT}" && pwd -P)"
OUTPUT="${OUTPUT_PARENT}/$(basename "${OUTPUT}")"

case "${WORK_DIR}/" in
    "${SOURCE}/"*)
        echo "work directory must not be inside the source checkout: ${WORK_DIR}" >&2
        exit 1
        ;;
esac
case "${OUTPUT}/" in
    "${SOURCE}/"*)
        echo "output must not be inside the source checkout: ${OUTPUT}" >&2
        exit 1
        ;;
esac

CHECKOUT="${WORK_DIR}/FluidX3D-${SHORT_REVISION}"
LOG_DIR="${WORK_DIR}/logs"
RELEASE_STAGE="${WORK_DIR}/release/FluidX3DSphereWake"
ARCHIVE="${OUTPUT}.release.tar.gz"
DESCRIPTOR="${OUTPUT%.cfdviz}.download.json"
WORK_MARKER="${WORK_DIR}/.flowviz-fluidx3d-generation"

if [[ -e "${WORK_DIR}" || -L "${WORK_DIR}" ]]; then
    if [[ "${FORCE}" -ne 1 ]]; then
        echo "generation work directory already exists; choose a new --work-dir or pass --force" >&2
        exit 1
    fi
    [[ -f "${WORK_MARKER}" ]] || {
        echo "refusing to replace unowned work directory: ${WORK_DIR}" >&2
        exit 1
    }
    rm -rf "${WORK_DIR}"
fi
if [[ -e "${OUTPUT}" || -L "${OUTPUT}" ]]; then
    if [[ "${FORCE}" -ne 1 ]]; then
        echo "generation output already exists; choose a new --output or pass --force" >&2
        exit 1
    fi
    OUTPUT_MANIFEST="${OUTPUT}/manifest.json"
    [[ -f "${OUTPUT_MANIFEST}" ]] || {
        echo "refusing to replace unowned output directory: ${OUTPUT}" >&2
        exit 1
    }
    PYTHONPATH="${HERE}/cfdviz/src" python3 - "${OUTPUT_MANIFEST}" "${REVISION}" <<'PY'
from __future__ import annotations

import json
import sys
from pathlib import Path

manifest = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
case = manifest.get("case", {})
solver = case.get("solver", {})
if not (
    case.get("quality") == "external-solver-sample"
    and solver.get("name") == "FluidX3D"
    and solver.get("commit") == sys.argv[2]
):
    raise SystemExit("output is not the pinned FlowViz FluidX3D sample")
PY
    rm -rf "${OUTPUT}"
fi
if [[ -e "${ARCHIVE}" || -L "${ARCHIVE}" ]]; then
    if [[ "${FORCE}" -ne 1 ]]; then
        echo "generation archive already exists; choose a new --output or pass --force" >&2
        exit 1
    fi
    [[ -f "${DESCRIPTOR}" ]] || {
        echo "refusing to replace an archive without its generated descriptor: ${ARCHIVE}" >&2
        exit 1
    }
    rm -f "${ARCHIVE}"
fi
mkdir -p "${WORK_DIR}" "${LOG_DIR}"
printf '%s\n' "${REVISION}" > "${WORK_MARKER}"

# --no-local prevents hard links and ensures the dirty source working tree is
# never treated as the generation checkout.
git clone --no-local "${SOURCE}" "${CHECKOUT}"
git -C "${CHECKOUT}" checkout --detach "${REVISION}"
[[ "$(git -C "${CHECKOUT}" rev-parse HEAD)" == "${REVISION}" ]] || {
    echo "isolated checkout did not land on ${REVISION}" >&2
    exit 1
}

cp "${SETUP_SOURCE}" "${CHECKOUT}/src/setup.cpp"
git -C "${CHECKOUT}" apply "${DEFINES_PATCH}"

JOBS="$(sysctl -n hw.logicalcpu 2>/dev/null || printf '4')"
make -C "${CHECKOUT}" macOS -j"${JOBS}" 2>&1 | tee "${LOG_DIR}/build.log"
[[ -x "${CHECKOUT}/bin/FluidX3D" ]] || {
    echo "FluidX3D build did not produce bin/FluidX3D" >&2
    exit 1
}

(
    cd "${CHECKOUT}"
    ./bin/FluidX3D
) 2>&1 | tee "${LOG_DIR}/simulation.log"

VTK_DIR="${CHECKOUT}/bin/export"
shopt -s nullglob
VELOCITY_FILES=("${VTK_DIR}"/u-*.vtk)
FLAG_FILES=("${VTK_DIR}"/flags-*.vtk)
shopt -u nullglob
if [[ "${#VELOCITY_FILES[@]}" -ne 40 || "${#FLAG_FILES[@]}" -ne 40 ]]; then
    echo "expected 40 velocity and 40 flag snapshots; found ${#VELOCITY_FILES[@]} and ${#FLAG_FILES[@]}" >&2
    exit 1
fi
[[ "$(basename "${VELOCITY_FILES[0]}")" == "u-000006000.vtk" ]] || {
    echo "first velocity snapshot is not solver step 6000" >&2
    exit 1
}
[[ "$(basename "${VELOCITY_FILES[39]}")" == "u-000006390.vtk" ]] || {
    echo "last velocity snapshot is not solver step 6390" >&2
    exit 1
}

cfdviz() {
    PYTHONPATH="${HERE}/cfdviz/src" python3 -m cfdviz "$@"
}

convert_case() {
    cfdviz import-fluidx3d \
        --output "${OUTPUT}" \
        --name "FluidX3D Sphere Wake" \
        --field U "${VELOCITY_FILES[@]}" \
        --flags "${VTK_DIR}"/flags-*.vtk \
        --source-time-step 1 \
        --solver-step-offset 0 \
        --source-units lattice \
        --source-axes +X +Y +Z \
        --solver-method "lattice-Boltzmann D3Q19 SRT" \
        --excluded-flag-bits 0x01 \
        --solver-commit "${REVISION}" \
        --solver-configuration \
            "D3Q19 SRT FP16S; equilibrium boundaries; 192x96x96; sphere D=24; Re=200; u=0.06; warmup=6000; stride=10" \
        --source-case "sphere-wake-192x96x96" \
        --float16 \
        --brick-size 32 32 32 \
        --codec zlib \
        --level 6 \
        --max-payload-bytes 50000000 \
        "$@"
}

if [[ "${FORCE}" -eq 1 ]]; then
    convert_case --force
else
    convert_case
fi
cfdviz validate "${OUTPUT}" | tee "${LOG_DIR}/validate.txt"
cfdviz known-values "${OUTPUT}" --check | tee "${LOG_DIR}/known-values.txt"
cfdviz inspect "${OUTPUT}" | tee "${LOG_DIR}/inspect.txt"
cfdviz qualify-representative "${OUTPUT}" \
    --solver FluidX3D \
    --revision "${REVISION}" \
    --minimum-active-dimensions 192 96 96 \
    --minimum-frames 40 \
    --minimum-spanwise-velocity-ratio 0.005 \
    --minimum-spanwise-gradient-ratio 0.005 \
    --minimum-temporal-change-ratio 0.00001 \
    --maximum-feature-displacement-cells 2 \
    --json > "${LOG_DIR}/qualification.json"
cfdviz benchmark-read "${OUTPUT}" --field U --repeat 1 --json \
    > "${LOG_DIR}/benchmark-read.json"

# Clause 5 of the FluidX3D license requires the complete altered source when
# generated data is published. The release stage therefore carries a reconstructable
# source tree, the exact recipe, logs, checksums, and the qualified CFDViz case.
mkdir -p \
    "${RELEASE_STAGE}/case" \
    "${RELEASE_STAGE}/altered-source/FluidX3D" \
    "${RELEASE_STAGE}/recipe" \
    "${RELEASE_STAGE}/evidence"
cp -R "${OUTPUT}/." "${RELEASE_STAGE}/case/"
cp "${SETUP_SOURCE}" "${DEFINES_PATCH}" "${RECIPE_README}" \
    "${RELEASE_STAGE}/recipe/"
cp "${LOG_DIR}/build.log" "${RELEASE_STAGE}/evidence/"
FLOWVIZ_EVIDENCE_DIR="${RELEASE_STAGE}/evidence" \
FLOWVIZ_QUALIFICATION="${LOG_DIR}/qualification.json" \
FLOWVIZ_BENCHMARK="${LOG_DIR}/benchmark-read.json" \
FLOWVIZ_VALIDATE="${LOG_DIR}/validate.txt" \
FLOWVIZ_KNOWN_VALUES="${LOG_DIR}/known-values.txt" \
FLOWVIZ_OUTPUT="${OUTPUT}" \
python3 - <<'PY'
from __future__ import annotations

import json
import os
from pathlib import Path

for source_name, target_name in (
    ("FLOWVIZ_QUALIFICATION", "qualification.json"),
    ("FLOWVIZ_BENCHMARK", "benchmark-read.json"),
):
    payload = json.loads(Path(os.environ[source_name]).read_text(encoding="utf-8"))
    if "root" in payload:
        payload["root"] = "case"
    if "case" in payload:
        payload["case"] = "case"
    (Path(os.environ["FLOWVIZ_EVIDENCE_DIR"]) / target_name).write_text(
        json.dumps(payload, indent=2, sort_keys=True, allow_nan=False) + "\n",
        encoding="utf-8",
    )

output = os.environ["FLOWVIZ_OUTPUT"]
evidence = Path(os.environ["FLOWVIZ_EVIDENCE_DIR"])
for source_name, target_name in (
    ("FLOWVIZ_VALIDATE", "validate.txt"),
    ("FLOWVIZ_KNOWN_VALUES", "known-values.txt"),
):
    text = Path(os.environ[source_name]).read_text(encoding="utf-8")
    evidence.joinpath(target_name).write_text(
        text.replace(output, "case"),
        encoding="utf-8",
    )
PY
COPYFILE_DISABLE=1 tar -C "${CHECKOUT}" \
    --exclude=.git --exclude=bin --exclude=temp -cf - . \
    | tar -C "${RELEASE_STAGE}/altered-source/FluidX3D" -xf -

FLOWVIZ_RELEASE_STAGE="${RELEASE_STAGE}" \
FLOWVIZ_VTK_DIR="${VTK_DIR}" \
FLOWVIZ_REVISION="${REVISION}" \
FLOWVIZ_QUALIFICATION="${LOG_DIR}/qualification.json" \
python3 - <<'PY'
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path

stage = Path(os.environ["FLOWVIZ_RELEASE_STAGE"])
vtk_dir = Path(os.environ["FLOWVIZ_VTK_DIR"])
qualification = json.loads(
    Path(os.environ["FLOWVIZ_QUALIFICATION"]).read_text(encoding="utf-8")
)
qualification["root"] = "case"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()

exports = []
for path in sorted((*vtk_dir.glob("u-*.vtk"), *vtk_dir.glob("flags-*.vtk"))):
    exports.append(
        {
            "path": path.name,
            "bytes": path.stat().st_size,
            "sha256": sha256(path),
        }
    )
metadata = {
    "schemaVersion": 1,
    "solver": {
        "name": "FluidX3D",
        "origin": "https://github.com/ProjectPhysX/FluidX3D",
        "revision": os.environ["FLOWVIZ_REVISION"],
        "alteredSource": "altered-source/FluidX3D",
        "license": "altered-source/FluidX3D/LICENSE.md",
    },
    "simulation": {
        "lattice": [192, 96, 96],
        "sphereDiameterCells": 24,
        "reynoldsNumber": 200,
        "inletVelocityCellsPerStep": 0.06,
        "warmupSteps": 6000,
        "storedFrames": 40,
        "storedStepStride": 10,
    },
    "sourceExports": exports,
    "qualification": qualification,
}
(stage / "generation.json").write_text(
    json.dumps(metadata, indent=2, sort_keys=True, allow_nan=False) + "\n",
    encoding="utf-8",
)
PY

COPYFILE_DISABLE=1 tar -C "$(dirname "${RELEASE_STAGE}")" \
    -cf - "$(basename "${RELEASE_STAGE}")" | gzip -n -1 > "${ARCHIVE}"
ARCHIVE_SHA256="$(shasum -a 256 "${ARCHIVE}" | cut -d' ' -f1)"
ARCHIVE_BYTES="$(stat -f '%z' "${ARCHIVE}" 2>/dev/null || stat -c '%s' "${ARCHIVE}")"

FLOWVIZ_DESCRIPTOR="${DESCRIPTOR}" \
FLOWVIZ_ARCHIVE="${ARCHIVE}" \
FLOWVIZ_ARCHIVE_SHA256="${ARCHIVE_SHA256}" \
FLOWVIZ_ARCHIVE_BYTES="${ARCHIVE_BYTES}" \
FLOWVIZ_REVISION="${REVISION}" \
python3 - <<'PY'
from __future__ import annotations

import json
import os
from pathlib import Path

payload = {
    "schemaVersion": 1,
    "archiveName": Path(os.environ["FLOWVIZ_ARCHIVE"]).name,
    "archiveBytes": int(os.environ["FLOWVIZ_ARCHIVE_BYTES"]),
    "archiveSha256": os.environ["FLOWVIZ_ARCHIVE_SHA256"],
    "url": None,
    "releaseRoot": "FluidX3DSphereWake",
    "casePath": "case",
    "caseDirectory": "FluidX3DSphereWake.cfdviz",
    "supportDirectory": "FluidX3DSphereWake.release",
    "qualification": {
        "solverName": "FluidX3D",
        "solverRevision": os.environ["FLOWVIZ_REVISION"],
        "minimumActiveDimensions": [192, 96, 96],
        "minimumFrames": 40,
        "minimumSpanwiseVelocityRatio": 0.005,
        "minimumSpanwiseGradientRatio": 0.005,
        "minimumTemporalChangeRatio": 0.00001,
        "maximumFeatureDisplacementCells": 2.0,
    },
    "solver": {
        "name": "FluidX3D",
        "revision": os.environ["FLOWVIZ_REVISION"],
    },
    "publicationStatus": (
        "release-ready locally; public URL intentionally unset pending FluidX3D "
        "license/use review and explicit publication approval"
    ),
}
Path(os.environ["FLOWVIZ_DESCRIPTOR"]).write_text(
    json.dumps(payload, indent=2, sort_keys=True) + "\n",
    encoding="utf-8",
)
PY

printf 'FluidX3D sample ready: %s\n' "${OUTPUT}"
printf 'Release archive: %s\n' "${ARCHIVE}"
printf 'Archive bytes: %s\n' "${ARCHIVE_BYTES}"
printf 'Archive sha256: %s\n' "${ARCHIVE_SHA256}"
printf 'Descriptor: %s\n' "${DESCRIPTOR}"
