#!/usr/bin/env bash
# Install the checksum-pinned representative CFD demo and its altered-source bundle.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FLOWVIZ_ROOT="$(cd "${HERE}/.." && pwd)"
DESCRIPTOR="${FLOWVIZ_ROOT}/Samples/FluidX3DSphereWake.download.json"
ARCHIVE=""
FORCE=0

usage() {
    cat <<EOF
Usage: Tools/download_demo_case.sh [options]

Options:
  --descriptor PATH  Download descriptor (default: Samples/FluidX3DSphereWake.download.json)
  --archive PATH     Use this local archive instead of the descriptor URL/cache
  --force            Replace an existing installed case and support bundle
  -h, --help         Show this help
EOF
}

while [[ "$#" -gt 0 ]]; do
    case "$1" in
        --descriptor)
            [[ "$#" -ge 2 ]] || { echo "--descriptor requires a path" >&2; exit 2; }
            DESCRIPTOR="$2"
            shift 2
            ;;
        --archive)
            [[ "$#" -ge 2 ]] || { echo "--archive requires a path" >&2; exit 2; }
            ARCHIVE="$2"
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

[[ -f "${DESCRIPTOR}" ]] || { echo "missing demo descriptor: ${DESCRIPTOR}" >&2; exit 1; }
DESCRIPTOR_DIR="$(cd "$(dirname "${DESCRIPTOR}")" && pwd -P)"
DESCRIPTOR="${DESCRIPTOR_DIR}/$(basename "${DESCRIPTOR}")"

# Parse once and emit shell-quoted assignments. Values remain data: shlex.quote
# prevents descriptor text from becoming shell syntax when evaluated below.
eval "$(python3 - "${DESCRIPTOR}" <<'PY'
from __future__ import annotations

import json
import shlex
import sys
from pathlib import Path, PurePosixPath

path = Path(sys.argv[1])
payload = json.loads(path.read_text(encoding="utf-8"))
if payload.get("schemaVersion") != 1:
    raise SystemExit(f"unsupported descriptor schemaVersion: {payload.get('schemaVersion')!r}")
required_strings = (
    "archiveName",
    "archiveSha256",
    "releaseRoot",
    "casePath",
    "caseDirectory",
    "supportDirectory",
)
for key in required_strings:
    value = payload.get(key)
    if not isinstance(value, str) or not value:
        raise SystemExit(f"descriptor {key} must be a non-empty string")
    pure = PurePosixPath(value)
    if pure.is_absolute() or ".." in pure.parts:
        raise SystemExit(f"descriptor {key} is not a safe relative path")
if not isinstance(payload.get("archiveBytes"), int) or payload["archiveBytes"] < 1:
    raise SystemExit("descriptor archiveBytes must be a positive integer")
if len(payload["archiveSha256"]) != 64:
    raise SystemExit("descriptor archiveSha256 must contain 64 hexadecimal characters")
try:
    int(payload["archiveSha256"], 16)
except ValueError as exc:
    raise SystemExit("descriptor archiveSha256 is not hexadecimal") from exc
qualification = payload.get("qualification")
if not isinstance(qualification, dict):
    raise SystemExit("descriptor qualification must be an object")
keys = {
    "SOLVER_NAME": "solverName",
    "SOLVER_REVISION": "solverRevision",
    "MINIMUM_FRAMES": "minimumFrames",
    "MINIMUM_SPANWISE_VELOCITY_RATIO": "minimumSpanwiseVelocityRatio",
    "MINIMUM_SPANWISE_GRADIENT_RATIO": "minimumSpanwiseGradientRatio",
    "MINIMUM_TEMPORAL_CHANGE_RATIO": "minimumTemporalChangeRatio",
    "MAXIMUM_FEATURE_DISPLACEMENT_CELLS": "maximumFeatureDisplacementCells",
}
assignments = {
    "ARCHIVE_NAME": payload["archiveName"],
    "EXPECTED_BYTES": str(payload["archiveBytes"]),
    "EXPECTED_SHA256": payload["archiveSha256"].lower(),
    "DOWNLOAD_URL": payload.get("url") or "",
    "RELEASE_ROOT": payload["releaseRoot"],
    "CASE_PATH": payload["casePath"],
    "CASE_DIRECTORY": payload["caseDirectory"],
    "SUPPORT_DIRECTORY": payload["supportDirectory"],
}
for shell_name, json_name in keys.items():
    if json_name not in qualification:
        raise SystemExit(f"descriptor qualification.{json_name} is absent")
    assignments[shell_name] = str(qualification[json_name])
dimensions = qualification.get("minimumActiveDimensions")
if not (
    isinstance(dimensions, list)
    and len(dimensions) == 3
    and all(isinstance(value, int) and value > 0 for value in dimensions)
):
    raise SystemExit("descriptor qualification.minimumActiveDimensions is malformed")
assignments["MINIMUM_ACTIVE_DIMENSIONS"] = " ".join(str(value) for value in dimensions)
for key, value in assignments.items():
    print(f"{key}={shlex.quote(value)}")
PY
)"

OUTPUT="${DESCRIPTOR_DIR}/${CASE_DIRECTORY}"
SUPPORT="${DESCRIPTOR_DIR}/${SUPPORT_DIRECTORY}"
if [[ -z "${ARCHIVE}" ]]; then
    ARCHIVE="${DESCRIPTOR_DIR}/${ARCHIVE_NAME}"
fi

if [[ ! -f "${ARCHIVE}" ]]; then
    if [[ -z "${DOWNLOAD_URL}" ]]; then
        echo "demo archive is not present locally and descriptor url is unset" >&2
        echo "generate it with Tools/generate_fluidx3d_sample.sh or publish the release archive after license review" >&2
        exit 1
    fi
    PARTIAL="${ARCHIVE}.partial"
    rm -f "${PARTIAL}"
    cleanup_partial() {
        rm -f "${PARTIAL}"
    }
    trap cleanup_partial EXIT INT TERM
    curl --fail --location --output "${PARTIAL}" "${DOWNLOAD_URL}"
    ACTUAL_BYTES="$(stat -f '%z' "${PARTIAL}" 2>/dev/null || stat -c '%s' "${PARTIAL}")"
    if [[ "${ACTUAL_BYTES}" != "${EXPECTED_BYTES}" ]]; then
        echo "downloaded archive size mismatch: expected ${EXPECTED_BYTES}, got ${ACTUAL_BYTES}" >&2
        exit 1
    fi
    ACTUAL_SHA256="$(shasum -a 256 "${PARTIAL}" | cut -d' ' -f1)"
    if [[ "${ACTUAL_SHA256}" != "${EXPECTED_SHA256}" ]]; then
        echo "downloaded archive checksum mismatch: expected ${EXPECTED_SHA256}, got ${ACTUAL_SHA256}" >&2
        exit 1
    fi
    mv "${PARTIAL}" "${ARCHIVE}"
    trap - EXIT INT TERM
fi

ACTUAL_BYTES="$(stat -f '%z' "${ARCHIVE}" 2>/dev/null || stat -c '%s' "${ARCHIVE}")"
if [[ "${ACTUAL_BYTES}" != "${EXPECTED_BYTES}" ]]; then
    echo "archive size mismatch: expected ${EXPECTED_BYTES}, got ${ACTUAL_BYTES}" >&2
    exit 1
fi
ACTUAL_SHA256="$(shasum -a 256 "${ARCHIVE}" | cut -d' ' -f1)"
if [[ "${ACTUAL_SHA256}" != "${EXPECTED_SHA256}" ]]; then
    echo "archive checksum mismatch: expected ${EXPECTED_SHA256}, got ${ACTUAL_SHA256}" >&2
    exit 1
fi

if [[ -e "${OUTPUT}" || -L "${OUTPUT}" || -e "${SUPPORT}" || -L "${SUPPORT}" ]]; then
    if [[ "${FORCE}" -ne 1 ]]; then
        echo "demo is already installed; pass --force to replace it" >&2
        exit 1
    fi
fi

STAGING="${DESCRIPTOR_DIR}/.${CASE_DIRECTORY}.install.$$"
BACKUP_CASE="${STAGING}/previous-case"
BACKUP_SUPPORT="${STAGING}/previous-support"
NEW_CASE_INSTALLED=0
NEW_SUPPORT_INSTALLED=0
rm -rf "${STAGING}"
mkdir -p "${STAGING}"
cleanup() {
    status="$?"
    trap - EXIT INT TERM
    if [[ "${NEW_SUPPORT_INSTALLED}" -eq 1 ]]; then
        rm -rf "${SUPPORT}"
    fi
    if [[ -e "${BACKUP_SUPPORT}" || -L "${BACKUP_SUPPORT}" ]]; then
        mv "${BACKUP_SUPPORT}" "${SUPPORT}" || true
    fi
    if [[ "${NEW_CASE_INSTALLED}" -eq 1 ]]; then
        rm -rf "${OUTPUT}"
    fi
    if [[ -e "${BACKUP_CASE}" || -L "${BACKUP_CASE}" ]]; then
        mv "${BACKUP_CASE}" "${OUTPUT}" || true
    fi
    rm -rf "${STAGING}"
    exit "${status}"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

python3 - "${ARCHIVE}" "${STAGING}" "${RELEASE_ROOT}" <<'PY'
from __future__ import annotations

import sys
import tarfile
from pathlib import Path, PurePosixPath

archive = Path(sys.argv[1])
staging = Path(sys.argv[2])
release_root = sys.argv[3]
with tarfile.open(archive, "r:gz") as stream:
    members = stream.getmembers()
    if not members:
        raise SystemExit("demo archive is empty")
    for member in members:
        pure = PurePosixPath(member.name)
        if pure.is_absolute() or ".." in pure.parts:
            raise SystemExit(f"unsafe archive path: {member.name}")
        if not pure.parts or pure.parts[0] != release_root:
            raise SystemExit(
                f"archive member {member.name!r} is outside release root {release_root!r}"
            )
        if member.issym() or member.islnk() or member.isdev():
            raise SystemExit(f"archive member is not a regular file/directory: {member.name}")
    stream.extractall(staging)
PY

STAGED_ROOT="${STAGING}/${RELEASE_ROOT}"
STAGED_CASE="${STAGED_ROOT}/${CASE_PATH}"
[[ -f "${STAGED_CASE}/manifest.json" ]] || {
    echo "archive does not contain ${RELEASE_ROOT}/${CASE_PATH}/manifest.json" >&2
    exit 1
}
for required in \
    "altered-source/FluidX3D/LICENSE.md" \
    "altered-source/FluidX3D/src/setup.cpp" \
    "altered-source/FluidX3D/src/defines.hpp" \
    "recipe/sphere_wake_setup.cpp" \
    "recipe/defines.patch" \
    "recipe/README.md" \
    "evidence/qualification.json" \
    "evidence/known-values.txt" \
    "generation.json"
do
    [[ -f "${STAGED_ROOT}/${required}" ]] || {
        echo "archive is missing required release support file: ${required}" >&2
        exit 1
    }
done

cfdviz() {
    PYTHONPATH="${HERE}/cfdviz/src" python3 -m cfdviz "$@"
}

cfdviz validate "${STAGED_CASE}" >/dev/null
cfdviz known-values "${STAGED_CASE}" --check >/dev/null
# shellcheck disable=SC2086 # exactly three descriptor-validated integers
cfdviz qualify-representative "${STAGED_CASE}" \
    --solver "${SOLVER_NAME}" \
    --revision "${SOLVER_REVISION}" \
    --minimum-active-dimensions ${MINIMUM_ACTIVE_DIMENSIONS} \
    --minimum-frames "${MINIMUM_FRAMES}" \
    --minimum-spanwise-velocity-ratio "${MINIMUM_SPANWISE_VELOCITY_RATIO}" \
    --minimum-spanwise-gradient-ratio "${MINIMUM_SPANWISE_GRADIENT_RATIO}" \
    --minimum-temporal-change-ratio "${MINIMUM_TEMPORAL_CHANGE_RATIO}" \
    --maximum-feature-displacement-cells "${MAXIMUM_FEATURE_DISPLACEMENT_CELLS}" \
    >/dev/null

if [[ -e "${OUTPUT}" || -L "${OUTPUT}" ]]; then
    mv "${OUTPUT}" "${BACKUP_CASE}"
fi
if [[ -e "${SUPPORT}" || -L "${SUPPORT}" ]]; then
    mv "${SUPPORT}" "${BACKUP_SUPPORT}"
fi

mv "${STAGED_CASE}" "${OUTPUT}"
NEW_CASE_INSTALLED=1
if ! mv "${STAGED_ROOT}" "${SUPPORT}"; then
    echo "could not install the altered-source support bundle" >&2
    exit 1
fi
NEW_SUPPORT_INSTALLED=1
rm -rf "${BACKUP_CASE}" "${BACKUP_SUPPORT}"
trap - EXIT INT TERM
rm -rf "${STAGING}"

printf 'installed representative demo: %s\n' "${OUTPUT}"
printf 'installed altered-source/evidence bundle: %s\n' "${SUPPORT}"
