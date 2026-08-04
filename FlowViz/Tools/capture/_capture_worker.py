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
          "capture_source": "FINAL_COLOR_LDR"
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
import traceback

import unreal


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
    """Read back the render target and return (max, unique_rgb_count, mean).

    This mirrors the external PIL/numpy verification so a shot can be judged
    before the process exits.
    """
    pixels = unreal.RenderingLibrary.read_render_target(world, render_target, False)
    if not pixels:
        return -1, 0, 0.0

    largest = 0
    total = 0
    distinct = set()
    for p in pixels:
        r, g, b = int(p.r), int(p.g), int(p.b)
        if r > largest:
            largest = r
        if g > largest:
            largest = g
        if b > largest:
            largest = b
        total += r + g + b
        distinct.add((r, g, b))

    mean = float(total) / float(len(pixels) * 3)
    return largest, len(distinct), mean


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

    largest, distinct, mean = measure(world, render_target)

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

    passed = bool(written and largest > 0 and distinct > 10)
    log(
        "%s -> max=%d unique=%d mean=%.3f bytes=%d %s"
        % (filename, largest, distinct, mean, size, "PASS" if passed else "FAIL")
    )

    return {
        "output": output,
        "map": shot.get("map"),
        "written": written,
        "bytes": size,
        "max": largest,
        "unique": distinct,
        "mean": round(mean, 3),
        "pass": passed,
    }


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
