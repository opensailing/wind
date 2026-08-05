"""
_capture_worker.py -- runs INSIDE Unreal (pythonscript commandlet). Not for direct use.

Driven by capture_frame.py, which writes a JSON job file and passes its path via
the FLOWVIZ_CAPTURE_JOBS environment variable.

Job file schema:
    {
      "shots": [
        {
          "map": "/Game/FlowViz/Maps/L_CapTest",
          "output": "/abs/path/out.png",
          "location": [x, y, z],
          "rotation": [pitch, yaw, roll],
          "resolution": [w, h],
          "fov": 75.0,
          "capture_source": "FINAL_COLOR_LDR",

          # Optional. Present => spawn a CFDViz case actor and judge the shot
          # on whether the RAY-MARCHER drew, not merely on whether anything did.
          "volume": {
            "case": "/abs/path/Case.cfdviz",
            "field": "speed",          # MUST be scalar; a vector marches nothing
            "frame": 0,
            "location": [x, y, z],
            "rotation": [pitch, yaw, roll],
            "draw_bounding_box": false
          }
        }
      ],
      "result": "/abs/path/result.json"
    }

Results are written to job["result"] as a JSON list of per-shot dicts with the
measured pixel statistics, so the caller can verify without trusting log output.

Shots are grouped by map so each map loads at most once per launch: map loading
and engine startup dominate wall time, individual captures are cheap.
"""

import json
import os
import sys
import traceback

import unreal

# Sibling module, pure Python and unit-tested outside the engine. The commandlet
# does not put this script's directory on sys.path.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verdict import Stats, judge, judge_marcher  # noqa: E402


# print() does not reach the Unreal log from the pythonscript commandlet.
def log(msg):
    unreal.log_warning("[flowviz-capture] %s" % msg)


CAPTURE_SOURCES = {
    "FINAL_COLOR_LDR": unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR,
    "FINAL_COLOR_HDR": unreal.SceneCaptureSource.SCS_FINAL_COLOR_HDR,
    "FINAL_TONE_CURVE_HDR": unreal.SceneCaptureSource.SCS_FINAL_TONE_CURVE_HDR,
    "SCENE_COLOR_HDR": unreal.SceneCaptureSource.SCS_SCENE_COLOR_HDR,
    "BASE_COLOR": unreal.SceneCaptureSource.SCS_BASE_COLOR,
    "NORMAL": unreal.SceneCaptureSource.SCS_NORMAL,
    "SCENE_DEPTH": unreal.SceneCaptureSource.SCS_SCENE_DEPTH,
}


def measure(world, render_target):
    """Read back the render target and return a verdict.Stats, or None.

    This mirrors the external PIL/numpy verification so a shot can be judged
    before the process exits.

    The checksum is position-dependent: a plain histogram would call two frames
    equal whenever an object merely MOVED, which would report "nothing
    rendered" for a scene that rendered fine.
    """
    pixels = unreal.RenderingLibrary.read_render_target(world, render_target, False)
    if not pixels:
        return Stats(largest=-1, distinct=0, mean=0.0, checksum=0)

    largest = 0
    total = 0
    distinct = set()
    checksum = 0
    for index, p in enumerate(pixels):
        r, g, b = int(p.r), int(p.g), int(p.b)
        if r > largest:
            largest = r
        if g > largest:
            largest = g
        if b > largest:
            largest = b
        total += r + g + b
        distinct.add((r, g, b))
        # Cheap, order-sensitive, and adequate here: this only ever has to
        # distinguish "these two frames differ" from "they do not".
        checksum = (checksum * 31 + (index + 1) * (r * 65536 + g * 256 + b)) & 0xFFFFFFFF

    mean = float(total) / float(len(pixels) * 3)
    return Stats(largest=largest, distinct=len(distinct), mean=mean, checksum=checksum)


def check_lighting(world):
    """Warn about the lighting mistakes that render a correct scene black.

    Every one of these was observed in this project and each produces an image
    indistinguishable from a broken capture path, which is what makes them
    expensive: the natural response is to go debugging the renderer.
    """
    try:
        lights = unreal.GameplayStatics.get_all_actors_of_class(world, unreal.DirectionalLight)
        sky_atmospheres = unreal.GameplayStatics.get_all_actors_of_class(
            world, unreal.SkyAtmosphere
        )

        if lights and not sky_atmospheres:
            # Without a SkyAtmosphere there is no sky and no ambient bounce, so
            # anything not directly lit reads as black.
            log("WARNING: map has a DirectionalLight but no SkyAtmosphere; "
                "expect a black background and unlit surfaces")

        for light in lights:
            pitch = light.get_actor_rotation().pitch
            # Positive pitch aims the light UP. With a SkyAtmosphere that is
            # night: the sun is below the horizon and the frame is black even
            # though the scene, camera and capture path are all correct.
            if pitch > 0.0:
                log("WARNING: DirectionalLight pitch is +%.1f (pointing UP). "
                    "With a SkyAtmosphere this renders as night. Use a NEGATIVE "
                    "pitch such as -45 to aim the sun downward." % pitch)

            mobility = light.light_component.get_editor_property("mobility")
            if mobility != unreal.ComponentMobility.MOVABLE:
                # Static/stationary lights need a built lighting pass, and this
                # project runs with r.AllowStaticLighting=False.
                log("WARNING: DirectionalLight mobility is %s, not MOVABLE; "
                    "without built lighting it contributes nothing" % mobility)
    except Exception:
        pass


def disable_editor_overlays(world):
    """Stop the renderer drawing editor-only HUD text into the captured pixels.

    r.SkyAtmosphere.EditorNotifications defaults to 1, and when a map contains a
    skydome MESH with a sky material the renderer composites the warning
    "YOUR SCENE CONTAINS A SKYDOME MESH WITH A SKY MATERIAL..." directly into
    scene colour (SkyAtmosphereRendering.cpp: RenderSkyAtmosphereEditorNotifications,
    called from the base pass). It lands in the render target, not in a separate
    editor viewport layer, so it survives into the PNG.

    This is worth guarding against permanently rather than fixing per map. The
    contamination is invisible to every statistical check this tool runs -- the
    text ADDS brightness and unique colours, so it pushes max, mean and unique
    UP and makes a bad capture look healthier. /Engine/Maps/Templates/Template_Default
    trips it; many real content maps using a SkySphere mesh will too.
    """
    try:
        unreal.SystemLibrary.execute_console_command(
            world, "r.SkyAtmosphere.EditorNotifications 0"
        )
    except Exception:
        log("WARNING: could not disable sky-atmosphere editor notifications; "
            "captures of maps with a skydome mesh may contain overlaid text")


def spawn_volume(world, spec):
    """Place a CFDViz case actor for this shot. Returns the actor, or None.

    WHY A SEPARATE VERDICT EXISTS FOR THESE SHOTS, and why the primitive-
    suppressed reference above is not enough on its own.

    The volume's scene proxy emits three things from one GetDynamicMeshElements:
    a debug wireframe box, a SOLID hull mesh drawn with GEngine->DebugMeshMaterial,
    and the ray-march dispatch. The reference capture suppresses all three at
    once, so "the shot differs from the reference" goes true the moment the hull
    rasterizes -- and the hull is drawn with NO flag guarding it. That criterion
    therefore cannot distinguish a working ray-marcher from a dead one, and
    because the hull is an opaque box, the frame it certifies even LOOKS like a
    rendered volume. Turning off `draw_bounding_box` does not help; only the
    wireframe is behind that flag.

    So a volume shot additionally captures a state with the ray-march dispatcher
    uninstalled and everything else -- actor, hull, box, camera, lighting --
    held fixed. Any pixel differing between those two came from the marcher and
    from nothing else. See verdict.judge_marcher.
    """
    case = spec.get("case")
    field = spec.get("field")
    if not case:
        log("volume spec has no 'case' path; no volume was placed")
        return None
    if not field:
        # Refused rather than defaulted, matching SpawnCaseActor. The manifest's
        # first field is typically a vector, whose bytes never reach the scalar
        # texture the marcher samples: it would load, upload, and march nothing.
        log("volume spec has no 'field'; refusing to guess, because the default "
            "is typically a vector field that marches nothing")
        return None

    location = unreal.Vector(*spec.get("location", [0.0, 0.0, 0.0]))
    pitch, yaw, roll = spec.get("rotation", [0.0, 0.0, 0.0])
    rotation = unreal.Rotator(roll, pitch, yaw)

    actor, error = unreal.FlowVizCaptureLibrary.spawn_case_actor(
        world,
        case,
        field,
        location,
        rotation,
        int(spec.get("frame", 0)),
        bool(spec.get("draw_bounding_box", False)),
    )

    if actor is None:
        # Loud, because every downstream symptom of this is a picture rather
        # than an error: the shot would simply look empty.
        log("VOLUME NOT PLACED: %s" % (error or "spawn_case_actor returned null"))
        return None

    # Reported because a volume outside the camera frustum photographs exactly
    # like a volume that failed to render, and the two have nothing in common as
    # fixes. The case transform includes a grid-origin translation and a metres
    # -> centimetres scale, so the actor's own location is NOT where the data is.
    try:
        origin, extent = actor.get_actor_bounds(only_colliding_components=False)
        log("placed volume: case=%s field=%s frame=%d box=%s "
            "world bounds origin=(%.1f, %.1f, %.1f) extent=(%.1f, %.1f, %.1f)"
            % (case, field, int(spec.get("frame", 0)),
               bool(spec.get("draw_bounding_box", False)),
               origin.x, origin.y, origin.z, extent.x, extent.y, extent.z))
    except Exception:
        log("placed volume: case=%s field=%s frame=%d box=%s (bounds unavailable)"
            % (case, field, int(spec.get("frame", 0)),
               bool(spec.get("draw_bounding_box", False))))
    return actor


def capture_one(world, actor_subsystem, shot):
    """Render a single shot and write the PNG. Returns a result dict."""
    width, height = shot.get("resolution", [1280, 720])
    location = unreal.Vector(*shot.get("location", [-400.0, 0.0, 150.0]))
    # Unreal's Rotator constructor is (roll, pitch, yaw); we accept the more
    # conventional (pitch, yaw, roll) ordering used by the editor UI.
    pitch, yaw, roll = shot.get("rotation", [0.0, 0.0, 0.0])
    rotation = unreal.Rotator(roll, pitch, yaw)
    fov = shot.get("fov", 75.0)
    source_name = shot.get("capture_source", "FINAL_COLOR_LDR")
    source = CAPTURE_SOURCES.get(source_name, unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
    warmups = int(shot.get("warmup_captures", 3))

    # Placed BEFORE the render target and the camera, so the volume's proxy and
    # its dynamic data have reached the render thread before the first capture.
    volume_actor = None
    if shot.get("volume"):
        volume_actor = spawn_volume(world, shot["volume"])

    # RGBA8_SRGB is required: export_render_target only emits PNG for 8-bit
    # formats (float formats silently take the EXR/HDR path instead).
    render_target = unreal.RenderingLibrary.create_render_target2d(
        world, width, height, unreal.TextureRenderTargetFormat.RTF_RGBA8_SRGB
    )

    # Spawned from C++ because every Blueprint spawn entry point is marked
    # BlueprintInternalUseOnly and so is invisible to Python. Constructing the
    # component directly in Python "works" but lands it in /Engine/Transient
    # with no world, where it renders black and reports success -- the exact
    # failure this tool exists to rule out.
    capture_actor = unreal.FlowVizCaptureLibrary.spawn_scene_capture2d(
        world, location, rotation, fov
    )
    component = capture_actor.capture_component2d
    component.set_editor_property("texture_target", render_target)
    component.set_editor_property("capture_source", source)
    component.set_editor_property("fov_angle", fov)
    component.set_editor_property("capture_every_frame", False)
    component.set_editor_property("capture_on_movement", False)

    # Materials load lazily and a primitive whose material has not resolved yet
    # renders BLACK -- not a default grey. Skipping this yields a correct sky
    # above a pitch-black floor, which reads as a broken renderer.
    try:
        unreal.FlowVizCaptureLibrary.resolve_materials(world)
    except Exception:
        pass

    # Push pending component registrations to the render thread. Harmless when
    # unnecessary; cheap insurance against capturing a stale scene.
    try:
        unreal.FlowVizCaptureLibrary.flush_scene_updates(world)
    except Exception:
        pass

    for _ in range(max(1, warmups)):
        component.capture_scene()

    scene_stats = measure(world, render_target)

    # The reference frame: identical camera, identical everything, with every
    # primitive suppressed. If the shot matches this, no geometry drew a single
    # pixel -- which no single-frame brightness metric can detect, because the
    # sky alone is bright and richly dithered. See verdict.py.
    reference_stats = None
    try:
        component.set_editor_property(
            "primitive_render_mode",
            unreal.SceneCapturePrimitiveRenderMode.PRM_USE_SHOW_ONLY_LIST,
        )
        # An empty show-only list renders no primitives at all.
        component.clear_show_only_components()
        for _ in range(max(1, warmups)):
            component.capture_scene()
        reference_stats = measure(world, render_target)
    except Exception:
        log("WARNING: could not capture the primitive-suppressed reference:\n%s"
            % traceback.format_exc())

    # Restore and re-render, so the PNG written below is the real shot rather
    # than the reference we just took.
    try:
        component.set_editor_property(
            "primitive_render_mode",
            unreal.SceneCapturePrimitiveRenderMode.PRM_LEGACY_SCENE_CAPTURE,
        )
        for _ in range(max(1, warmups)):
            component.capture_scene()
    except Exception:
        pass

    # THE MARCHER-SUPPRESSED CONTROL. Only meaningful when a volume is in the
    # shot, so it is captured here rather than unconditionally -- see the long
    # comment on spawn_volume() for why the primitive-suppressed reference above
    # cannot answer this question on its own.
    without_marcher_stats = None
    if volume_actor is not None:
        try:
            still_on = unreal.FlowVizCaptureLibrary.set_volume_ray_marcher_enabled(False)
            if still_on:
                # The toggle did not take. Leaving without_marcher_stats as None
                # makes judge_marcher return UNSCORED, which is the honest
                # answer: capturing anyway would compare two identical states
                # and report "the marcher contributed nothing" -- a finding
                # about the harness dressed up as a finding about the renderer.
                log("WARNING: could not uninstall the ray-march dispatcher; "
                    "the marcher control was NOT captured")
            else:
                for _ in range(max(1, warmups)):
                    component.capture_scene()
                without_marcher_stats = measure(world, render_target)
        except Exception:
            log("WARNING: could not capture the marcher-suppressed control:\n%s"
                % traceback.format_exc())
        finally:
            # Restore unconditionally. A leaked-off dispatcher would silently
            # make every LATER shot in this batch marcher-free, and those shots
            # would fail with a reason that points at the renderer.
            try:
                restored = unreal.FlowVizCaptureLibrary.set_volume_ray_marcher_enabled(True)
                if not restored:
                    log("WARNING: could not reinstall the ray-march dispatcher; "
                        "subsequent shots in this batch are UNTRUSTWORTHY")
            except Exception:
                log("WARNING: failed to restore the ray-march dispatcher:\n%s"
                    % traceback.format_exc())

        # Re-render the real shot, so the PNG written below is state A and not
        # the control we just took.
        try:
            for _ in range(max(1, warmups)):
                component.capture_scene()
        except Exception:
            pass

    output = shot["output"]
    directory = os.path.dirname(os.path.abspath(output))
    filename = os.path.basename(output)
    if not os.path.isdir(directory):
        os.makedirs(directory)

    # export_render_target takes a directory and a filename separately, and
    # fails silently if the render target is null -- hence the checks above.
    unreal.RenderingLibrary.export_render_target(world, render_target, directory, filename)

    written = os.path.exists(output)
    size = os.path.getsize(output) if written else 0

    try:
        actor_subsystem.destroy_actor(capture_actor)
    except Exception:
        pass

    # Destroyed too, so shots do not accumulate volumes: a later shot in the
    # same map would otherwise capture every earlier shot's case actor as well.
    if volume_actor is not None:
        try:
            actor_subsystem.destroy_actor(volume_actor)
        except Exception:
            pass

    # A volume shot is held to the stricter criterion: it must be the MARCHER
    # that drew, not merely something. judge_marcher runs judge() first, so the
    # basic checks are not skipped -- it adds a requirement, it does not replace
    # one.
    if shot.get("volume"):
        verdict = judge_marcher(
            with_marcher=scene_stats,
            without_marcher=without_marcher_stats,
            reference=reference_stats,
            written=written,
            byte_size=size,
        )
    else:
        verdict = judge(
            scene=scene_stats, reference=reference_stats, written=written, byte_size=size
        )

    log(
        "%s -> max=%d unique=%d mean=%.3f bytes=%d %s"
        % (
            filename,
            scene_stats.largest,
            scene_stats.distinct,
            scene_stats.mean,
            size,
            "PASS" if verdict.passed else "FAIL",
        )
    )
    if not verdict.passed:
        log("  %s" % verdict.reason)

    result = {
        "output": output,
        "map": shot.get("map"),
        "written": written,
        "bytes": size,
        "pass": verdict.passed,
        "reason": verdict.reason,
    }
    result.update(scene_stats.as_dict())
    if reference_stats is not None:
        result["reference"] = reference_stats.as_dict()
    if shot.get("volume"):
        # Recorded even when None, so a reader can tell "the control was not
        # taken" from "the control was taken and matched" -- opposite diagnoses.
        result["volume"] = dict(shot["volume"])
        result["volume_placed"] = volume_actor is not None
        result["without_marcher"] = (
            without_marcher_stats.as_dict() if without_marcher_stats is not None else None
        )
    return result


def main():
    job_path = os.environ.get("FLOWVIZ_CAPTURE_JOBS")
    if not job_path or not os.path.exists(job_path):
        log("FLOWVIZ_CAPTURE_JOBS not set or missing: %r" % job_path)
        return

    with open(job_path, "r") as handle:
        job = json.load(handle)

    shots = job.get("shots", [])
    results = []

    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    editor_subsystem = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)

    # Group by map so each map is loaded once regardless of shot count.
    by_map = {}
    order = []
    for shot in shots:
        map_path = shot["map"]
        if map_path not in by_map:
            by_map[map_path] = []
            order.append(map_path)
        by_map[map_path].append(shot)

    for map_path in order:
        log("loading map %s" % map_path)
        try:
            unreal.EditorLoadingAndSavingUtils.load_map(map_path)
        except Exception:
            log("failed to load %s:\n%s" % (map_path, traceback.format_exc()))
            for shot in by_map[map_path]:
                results.append(
                    {
                        "output": shot["output"],
                        "map": map_path,
                        "written": False,
                        "bytes": 0,
                        "max": -1,
                        "unique": 0,
                        "mean": 0.0,
                        "pass": False,
                        "error": "map load failed",
                    }
                )
            continue

        world = editor_subsystem.get_editor_world()

        try:
            # Queries the render scene itself, so 0 genuinely means "nothing to
            # draw" and -1 means "could not tell" -- never conflate the two.
            count = unreal.FlowVizCaptureLibrary.get_scene_proxy_count(world)
            log("map %s render scene holds %d primitive proxies" % (map_path, count))
            if count == 0:
                log("WARNING: scene is empty; any capture here shows sky/fog only")
            unreal.FlowVizCaptureLibrary.log_primitive_breakdown(world)
        except Exception:
            pass

        # Must run after the map is loaded (needs a world for the console
        # command) and before any capture, since the cvar is sampled on the
        # render thread during the base pass.
        disable_editor_overlays(world)
        check_lighting(world)

        for shot in by_map[map_path]:
            try:
                results.append(capture_one(world, actor_subsystem, shot))
            except Exception:
                log("shot failed:\n%s" % traceback.format_exc())
                results.append(
                    {
                        "output": shot.get("output"),
                        "map": map_path,
                        "written": False,
                        "bytes": 0,
                        "max": -1,
                        "unique": 0,
                        "mean": 0.0,
                        "pass": False,
                        "error": traceback.format_exc(),
                    }
                )

    result_path = job.get("result")
    if result_path:
        with open(result_path, "w") as handle:
            json.dump(results, handle, indent=2)

    passed = sum(1 for r in results if r.get("pass"))
    log("complete: %d/%d shots passed" % (passed, len(results)))


main()
