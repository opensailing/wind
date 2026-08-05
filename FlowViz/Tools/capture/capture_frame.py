#!/usr/bin/env python3
"""
capture_frame.py -- headless frame capture for the FlowViz Unreal project (macOS/Metal).

Renders a map from an arbitrary camera and writes a PNG, with no editor window
and no user interaction. Intended to be called repeatedly by other agents to
capture scenes for visual review.

QUICK START

    # single shot
    ./capture_frame.py --map /Game/FlowViz/Maps/L_CapTest \
                       --output /tmp/shot.png \
                       --location -400 0 150 \
                       --rotation -15 0 0 \
                       --resolution 1280 720

    # many shots in ONE engine launch (strongly preferred -- see BATCHING)
    ./capture_frame.py --batch shots.json

EXIT CODE
    0  every requested PNG exists and contains real shaded content
    1  at least one shot failed verification

A shot PASSES only if it DIFFERS from the same frame rendered with every
primitive suppressed. Brightness is not the criterion and cannot be: a
SkyAtmosphere fills the frame with a bright dithered gradient using no geometry
at all, so an empty capture clears any threshold. See verdict.py.

When a shot fails, the reason is printed with it and included in --json output
as "reason".

Verification happens inside Unreal by reading the render target back, so a
"success" log line can never be mistaken for a file that was never written --
that exact failure mode is what this tool was built to eliminate.

BATCHING

Engine startup costs 90-120s; an individual capture costs well under a second.
Batching therefore matters enormously: 20 separate invocations take ~35 minutes,
while one batch of 20 takes ~2 minutes. Shots are grouped by map internally so
each map loads at most once.

The batch file is a JSON list of shot objects (or an object with a "shots" key):

    [
      {
        "map": "/Game/FlowViz/Maps/L_CapTest",
        "output": "/tmp/front.png",
        "location": [-400, 0, 150],
        "rotation": [-15, 0, 0],
        "resolution": [1280, 720],
        "fov": 75.0,
        "capture_source": "FINAL_COLOR_LDR"
      }
    ]

Only "map" and "output" are required; everything else falls back to the
defaults documented in --help. Rotation is [pitch, yaw, roll] in degrees,
matching the editor's Details panel ordering.

capture_source accepts FINAL_COLOR_LDR (default, fully lit and tonemapped),
FINAL_TONE_CURVE_HDR, SCENE_COLOR_HDR, BASE_COLOR, NORMAL and SCENE_DEPTH.
Use FINAL_COLOR_LDR unless you specifically want a debug buffer; note that
BASE_COLOR and SCENE_DEPTH do not survive the 8-bit PNG path in a useful form
and are only meaningful for diagnostics.

REQUIREMENTS

Rendering must be explicitly enabled for the commandlet with
-AllowCommandletRendering; without it the RHI never initializes and every
capture is silently black. This script always passes it.

See README.md for verified measurements, per-capture timings, and known
content-authoring gotchas (notably that scenes must be lit by Movable lights).
"""

import argparse
import json
import math
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.abspath(os.path.join(HERE, "..", "..", "FlowViz.uproject"))
ENGINE_DIR = "/Users/Shared/Epic Games/UE_5.8"
EDITOR_CMD = os.path.join(ENGINE_DIR, "Engine", "Binaries", "Mac", "UnrealEditor-Cmd")
WORKER = os.path.join(HERE, "_capture_worker.py")

DEFAULT_RESOLUTION = (1280, 720)
DEFAULT_LOCATION = (-400.0, 0.0, 150.0)
DEFAULT_ROTATION = (-15.0, 0.0, 0.0)
DEFAULT_FOV = 75.0
DEFAULT_SOURCE = "FINAL_COLOR_LDR"


def look_at_rotation(location, target):
    """Return [pitch, yaw, roll] aiming from location at target.

    Hand-picked pitch/yaw values are the most common way to get a technically
    perfect capture of empty sky: the camera simply misses the geometry, and
    because a lit sky is bright and richly dithered, the result passes any
    "is it black?" check convincingly. Aiming at a known point removes the
    guesswork.
    """
    dx = target[0] - location[0]
    dy = target[1] - location[1]
    dz = target[2] - location[2]

    yaw = math.degrees(math.atan2(dy, dx))
    pitch = math.degrees(math.atan2(dz, math.hypot(dx, dy)))
    return [pitch, yaw, 0.0]


def normalize(shot):
    """Fill in defaults and make the output path absolute."""
    if "map" not in shot or "output" not in shot:
        raise ValueError("each shot needs 'map' and 'output': %r" % (shot,))

    location = list(shot.get("location", DEFAULT_LOCATION))

    # look_at wins over rotation when both are given; it is the more specific
    # request and the one far less likely to be wrong.
    if shot.get("look_at") is not None:
        rotation = look_at_rotation(location, list(shot["look_at"]))
    else:
        rotation = list(shot.get("rotation", DEFAULT_ROTATION))

    normalized = {
        "map": shot["map"],
        "output": os.path.abspath(os.path.expanduser(shot["output"])),
        "location": location,
        "rotation": rotation,
        "resolution": list(shot.get("resolution", DEFAULT_RESOLUTION)),
        "fov": float(shot.get("fov", DEFAULT_FOV)),
        "capture_source": shot.get("capture_source", DEFAULT_SOURCE),
        "warmup_captures": int(shot.get("warmup_captures", 3)),
    }

    # Forwarded explicitly. This function builds its result by naming the keys it
    # wants, so anything unnamed is dropped WITHOUT an error and the shot still
    # renders -- producing a capture with no volume in it, which is
    # pixel-indistinguishable from a capture whose volume failed to render.
    volume = shot.get("volume")
    if volume is not None:
        for required in ("case", "field"):
            if not volume.get(required):
                # Rejected here, where the caller still has a stack trace.
                # Downstream both of these degrade into a picture: no case means
                # no volume at all, and the worker refuses to guess a field
                # because the manifest's first one is typically a vector, which
                # loads and uploads happily and then marches nothing.
                raise ValueError(
                    "a volume spec needs %r; without it the shot renders an "
                    "empty room and reports success: %r" % (required, volume)
                )
        # Copied, so a caller reusing one spec across several shots cannot have a
        # later edit reach backwards into an earlier one.
        normalized["volume"] = dict(volume)

        # Render settings, validated but NOT defaulted.
        #
        # Absent stays absent: the worker only writes a setting the shot asked
        # for, so a shot omitting these renders exactly as it did before the
        # controls existed -- which is what every reference image in the repo
        # was captured with.
        #
        # Validated because the ENGINE'S refusal is invisible. SetVolumeCompositeMode
        # rejects an out-of-range mode and keeps the previous one, by design: a
        # control given a bad number must not become a control that does nothing.
        # For a capture script that is the worst possible outcome -- the shot
        # completes, the PNG holds a plausible volume, and it is the wrong mode.
        # No pixel check, reference diff or blind critic can detect that
        # afterwards, so it is caught here where the caller still has a stack
        # trace.
        mode = volume.get("composite_mode")
        if mode is not None:
            # Matches EFlowVizCompositeMode and the FLOWVIZ_MODE_* defines in
            # the .usf: 0 Alpha, 1 Maximum, 2 Minimum, 3 Average, 4 IsoSurface,
            # 5 Diagnostic.
            if not isinstance(mode, int) or isinstance(mode, bool) or not 0 <= mode <= 5:
                raise ValueError(
                    "composite_mode must be 0-5 (0 alpha, 1 max, 2 min, 3 avg, "
                    "4 iso, 5 diag), got %r. The engine would refuse this and "
                    "render the PREVIOUS mode, producing a successful capture "
                    "of the wrong thing." % (mode,)
                )

        iso = volume.get("iso_value")
        if iso is not None:
            iso = float(iso)
            # NaN compares false against everything, so an iso-surface at NaN
            # finds no crossing and renders empty -- the same picture as a
            # threshold outside the data range, and a different fix.
            if not math.isfinite(iso):
                raise ValueError(
                    "iso_value must be finite, got %r. A non-finite threshold "
                    "renders an EMPTY iso-surface, which looks exactly like a "
                    "threshold outside the data range." % (volume.get("iso_value"),)
                )

    return normalized


def capture(shots, project=PROJECT, editor=EDITOR_CMD, verbose=False, timeout=1800):
    """Render every shot in a single engine launch.

    Returns the list of per-shot result dicts produced by the in-engine worker.
    Importable: other tools can call this directly instead of shelling out.
    """
    shots = [normalize(s) for s in shots]

    if not os.path.exists(editor):
        raise RuntimeError("editor binary not found: %s" % editor)
    if not os.path.exists(project):
        raise RuntimeError("uproject not found: %s" % project)

    for shot in shots:
        directory = os.path.dirname(shot["output"])
        if not os.path.isdir(directory):
            os.makedirs(directory)
        # Remove any stale file so existence alone is meaningful evidence.
        if os.path.exists(shot["output"]):
            os.remove(shot["output"])

    workdir = tempfile.mkdtemp(prefix="flowviz-capture-")
    job_path = os.path.join(workdir, "jobs.json")
    result_path = os.path.join(workdir, "results.json")
    log_path = os.path.join(workdir, "engine.log")

    with open(job_path, "w") as handle:
        json.dump({"shots": shots, "result": result_path}, handle, indent=2)

    command = [
        editor,
        project,
        "-run=pythonscript",
        "-script=%s" % WORKER,
        # Without this the renderer is disabled in commandlets and every
        # captured frame is black. This is the single most important flag.
        "-AllowCommandletRendering",
        "-unattended",
        "-nopause",
        "-nosplash",
        "-stdout",
        "-AbsLog=%s" % log_path,
    ]

    environment = dict(os.environ)
    environment["FLOWVIZ_CAPTURE_JOBS"] = job_path

    started = time.time()
    process = subprocess.run(
        command,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
    )
    elapsed = time.time() - started

    output = process.stdout.decode("utf-8", "replace")
    if verbose:
        sys.stderr.write(output)
    else:
        for line in output.splitlines():
            if "[flowviz-capture]" in line and "LogInit: Display" not in line:
                sys.stderr.write(line.split("LogPython: Warning: ")[-1] + "\n")

    if os.path.exists(result_path):
        with open(result_path, "r") as handle:
            results = json.load(handle)
    else:
        results = [
            dict(shot, written=False, bytes=0, max=-1, unique=0, mean=0.0, **{"pass": False})
            for shot in shots
        ]
        sys.stderr.write(
            "no results produced; engine log kept at %s\n" % log_path
        )

    # Trust the file on disk over anything the engine reported.
    for result in results:
        path = result.get("output")
        if path and os.path.exists(path):
            result["written"] = True
            result["bytes"] = os.path.getsize(path)
        else:
            result["written"] = False
            result["pass"] = False

    sys.stderr.write(
        "captured %d shot(s) in %.1fs (%.1fs/shot amortized)\n"
        % (len(results), elapsed, elapsed / max(1, len(results)))
    )
    if not all(r.get("pass") for r in results):
        sys.stderr.write("engine log: %s\n" % log_path)

    return results


def main():
    parser = argparse.ArgumentParser(
        description="Render an Unreal map headlessly to a PNG.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--map", help="package path, e.g. /Game/FlowViz/Maps/L_CapTest")
    parser.add_argument("--output", help="destination PNG path")
    parser.add_argument(
        "--location", nargs=3, type=float, metavar=("X", "Y", "Z"),
        default=list(DEFAULT_LOCATION), help="camera position (default: %(default)s)",
    )
    parser.add_argument(
        "--rotation", nargs=3, type=float, metavar=("PITCH", "YAW", "ROLL"),
        default=list(DEFAULT_ROTATION), help="camera rotation in degrees (default: %(default)s)",
    )
    parser.add_argument(
        "--look-at", nargs=3, type=float, metavar=("X", "Y", "Z"), default=None,
        help="aim the camera at this point, overriding --rotation. Strongly "
             "preferred: a mis-aimed camera captures bright empty sky, which "
             "passes every brightness check while showing no geometry at all",
    )
    parser.add_argument(
        "--resolution", nargs=2, type=int, metavar=("W", "H"),
        default=list(DEFAULT_RESOLUTION), help="output size (default: %(default)s)",
    )
    parser.add_argument("--fov", type=float, default=DEFAULT_FOV, help="horizontal FOV (default: %(default)s)")
    parser.add_argument(
        "--capture-source", default=DEFAULT_SOURCE,
        help="FINAL_COLOR_LDR, FINAL_TONE_CURVE_HDR, SCENE_COLOR_HDR, BASE_COLOR, NORMAL, SCENE_DEPTH",
    )
    parser.add_argument("--batch", help="JSON file of shots to render in one launch")
    parser.add_argument("--project", default=PROJECT, help="path to the .uproject")
    parser.add_argument("--editor", default=EDITOR_CMD, help="path to UnrealEditor-Cmd")
    parser.add_argument("--timeout", type=int, default=1800, help="seconds before giving up")
    parser.add_argument("--verbose", action="store_true", help="stream the full engine log")
    parser.add_argument("--json", action="store_true", help="print results as JSON on stdout")

    args = parser.parse_args()

    if args.batch:
        with open(args.batch, "r") as handle:
            payload = json.load(handle)
        shots = payload["shots"] if isinstance(payload, dict) else payload
    else:
        if not args.map or not args.output:
            parser.error("--map and --output are required unless --batch is used")
        shots = [
            {
                "map": args.map,
                "output": args.output,
                "location": args.location,
                "rotation": args.rotation,
                "look_at": args.look_at,
                "resolution": args.resolution,
                "fov": args.fov,
                "capture_source": args.capture_source,
            }
        ]

    results = capture(
        shots,
        project=args.project,
        editor=args.editor,
        verbose=args.verbose,
        timeout=args.timeout,
    )

    if args.json:
        print(json.dumps(results, indent=2))
    else:
        for result in results:
            print(
                "%s %s  max=%s unique=%s mean=%s"
                % (
                    "PASS" if result.get("pass") else "FAIL",
                    result.get("output"),
                    result.get("max"),
                    result.get("unique"),
                    result.get("mean"),
                )
            )

    return 0 if all(r.get("pass") for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
