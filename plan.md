You are a principal Unreal Engine rendering engineer, scientific-visualization engineer, and senior C++ systems architect.

Build a working Unreal Engine 5 scientific visualization application for time-varying CFD data, with an extensible path toward coupled CFD/FEA visualization.

The product name for now is “FlowViz.” Use that name consistently in modules, classes, directories, documentation, and UI. Keep naming easy to change later.

Do not stop at architecture diagrams or scaffolding. Deliver a functioning vertical slice that:

1. Reads a documented, language-neutral CFDViz case format.
2. Loads a time-varying mock CFD case from disk.
3. Uploads fields to the GPU asynchronously.
4. Displays an animated scalar volume.
5. Displays arbitrary slices, clipping, vector glyphs, and streamlines.
6. Provides a desktop scientific-visualization UI.
7. Provides timeline playback and scrubbing.
8. Provides probes, charts, color maps, opacity transfer functions, legends, and export.
9. Saves and reloads visualization sessions.
10. Builds as a standalone packaged Unreal application, not merely as an Editor utility.

Do not ask me for routine implementation choices. Inspect the repository and installed Unreal Engine environment, make reasonable decisions, document them, and continue. Do not claim success without compiling and running the tests available in the environment.

======================================================================
1. PRODUCT GOAL
======================================================================

FlowViz is a scientific visualization front end for a future state-of-the-art lattice-Boltzmann CFD and FEA solver.

The numerical solver will remain the authoritative source of truth. Unreal Engine is responsible for:

- Data ingestion and validation
- Temporal playback and interpolation
- GPU streaming and caching
- Scientific rendering
- Interactive filters
- Probes and quantitative inspection
- Camera and scene interaction
- Presentation and cinematic output
- Future immersive, remote, and collaborative workflows

Do not implement a production CFD solver inside Unreal Engine.

Do not use Niagara Fluids, Chaos, or Unreal’s physics systems as substitutes for solver results. Niagara may be used to render particles and pathlines driven by imported velocity fields.

The initial release supports a uniform Cartesian CFD grid. The file format and software architecture must leave room for:

- Sparse Cartesian grids
- Adaptive mesh refinement
- Multiple fluid regions
- Unstructured grids
- Lagrangian particles
- Static and moving boundaries
- FEA meshes
- Nodal displacement
- Element, nodal, and integration-point result fields
- Two-way fluid-structure interaction
- Live solver streaming

======================================================================
2. TARGET AND COMPATIBILITY
======================================================================

Primary target:

- Unreal Engine 5.8
- C++ project
- Desktop packaged application
- Windows and macOS where the selected Unreal rendering APIs permit
- Shader code written through Unreal’s cross-platform rendering abstractions
- Development and DebugGame Editor builds
- Packaged Development build for the current host platform

If the environment has Unreal Engine 5.6 or 5.7 instead of 5.8:

- Isolate version-specific rendering calls behind compatibility wrappers.
- Do not downgrade the data model or architecture.
- Record the tested engine version in README.md.
- Do not scatter engine-version preprocessor checks throughout the codebase.

Use C++ for:

- File parsing
- Data validation
- Data model
- Timeline
- Caching
- Frame loading
- Decompression
- GPU resource management
- Field sampling
- Pipeline state
- Expensive calculations
- Rendering components
- Session serialization

Use UMG, Slate, materials, Niagara, and Blueprints where appropriate for presentation and composition. Do not put core data logic in Blueprint graphs.

No Editor-only dependency may leak into the runtime module.

======================================================================
3. REQUIRED REPOSITORY STRUCTURE
======================================================================

Create or adapt the project toward this structure:

FlowViz/
  FlowViz.uproject
  Config/
  Content/
    FlowViz/
      Maps/
      Materials/
      Niagara/
      UI/
      Icons/
      Samples/
  Plugins/
    FlowVizRuntime/
      FlowVizRuntime.uplugin
      Shaders/
      Source/
        FlowVizRuntime/
          Public/
          Private/
    FlowVizEditor/
      FlowVizEditor.uplugin
      Source/
        FlowVizEditor/
          Public/
          Private/
  Source/
    FlowVizApp/
  Tools/
    cfdviz/
      pyproject.toml
      src/cfdviz/
      tests/
  Samples/
    MockCylinderWake.cfdviz/
  Docs/
    ARCHITECTURE.md
    CFDVIZ_FORMAT.md
    OPENFOAM_PARAVIEW_PARITY.md
    UI_CONTROLS.md
    PERFORMANCE.md
    BUILD.md
    BACKLOG.md
    THIRD_PARTY_NOTICES.md
    ADR/
      001-data-format.md
      002-runtime-volume-rendering.md
      003-time-interpolation.md
      004-coordinate-systems.md

The sample data may be generated during setup instead of committing a very large dataset. A low-resolution, immediately runnable sample must nevertheless be available to the packaged application.

======================================================================
4. NON-NEGOTIABLE ENGINEERING RULES
======================================================================

1. Never perform disk reads, decompression, full-field computation, or mesh generation on the game thread.

2. Never call UpdateResource or recreate Unreal texture assets every display frame unless used only as a documented fallback.

3. Use double- or triple-buffered GPU resources.

4. Keep physical data in solver units. Unit conversion belongs in the visualization adapter and display layer.

5. Do not silently normalize, quantize, clamp, smooth, or discard field values.

6. Any quantization must be declared in the manifest.

7. Any visual temporal interpolation must be visibly identified as interpolation.

8. Stable global color ranges must be the default for animations. Per-frame automatic ranges are available but must be clearly labeled because they can create misleading apparent changes.

9. Numeric probes must sample the underlying field, not the rendered color texture.

10. NaNs, invalid values, and masked cells must be handled explicitly and must never be converted silently to zero.

11. Pseudocolored scientific surfaces should default to neutral or unlit shading so lighting does not corrupt the apparent scalar color.

12. All package offsets, dimensions, counts, and uncompressed sizes must be bounds-checked before allocation.

13. A malformed or partially corrupted case must produce a useful error message instead of a crash.

14. Expensive interactive updates must be debounced and cancellable.

15. Do not ship nonfunctional buttons. Hide deferred controls or clearly mark them as unavailable.

======================================================================
5. APPLICATION ARCHITECTURE
======================================================================

Create the following conceptual layers.

A. Data source layer

Define an interface similar to:

ICFDVizDataSource
  Open(SourceUri)
  Close()
  GetManifest()
  RequestFrame(FrameRequest)
  CancelRequest(RequestId)
  GetAvailability()
  GetSourceStatistics()

Implement:

- FCFDVizFileDataSource
- FCFDVizMockDataSource, if useful for testing

Reserve interfaces for:

- FCFDVizLiveDataSource
- FCFDVizOpenFOAMAdapter
- FCFDVizVTKAdapter

Do not implement a direct OpenFOAM parser in the initial release. The core visualizer must not depend on OpenFOAM libraries.

B. Data model layer

Create types similar to:

- FCFDVizCaseManifest
- FCFDVizCaseMetadata
- FCFDVizCoordinateSystem
- FCFDVizTimeline
- FCFDVizGridDescriptor
- FCFDVizFieldDescriptor
- FCFDVizMeshDescriptor
- FCFDVizBoundaryPatch
- FCFDVizFrameDescriptor
- FCFDVizFieldStatistics
- FCFDVizTransferFunctionPreset
- FCFDVizFrameRequest
- FCFDVizFrameData

Expose a UObject-facing case object:

- UCFDVizCase

C. Runtime subsystem

Create:

- UCFDVizSubsystem, preferably a GameInstanceSubsystem
- FCFDVizFrameCache
- FCFDVizRequestScheduler
- FCFDVizVolumeTextureUploader
- FCFDVizCpuFieldSampler
- FCFDVizDerivedFieldService
- FCFDVizSessionManager

D. Pipeline layer

Create a serializable visualization pipeline/DAG.

Base node:

- UCFDVizPipelineNode

Required node types:

- UCFDVizCaseSourceNode
- UCFDVizVolumeNode
- UCFDVizSliceNode
- UCFDVizClipNode
- UCFDVizIsoSurfaceNode
- UCFDVizGlyphNode
- UCFDVizStreamlineNode
- UCFDVizParticleNode
- UCFDVizThresholdNode
- UCFDVizPointProbeNode
- UCFDVizLineProbeNode
- UCFDVizHistogramNode

Each node must have:

- Stable UUID
- Name
- Input node or source
- Visibility
- Enabled state
- Serializable settings
- Validation state
- Error state
- Dirty state
- Recompute/cancel behavior
- Basic and advanced properties
- Reset-to-default support

A user must be able to create multiple visualizations from one case, such as:

Case
  ├── Volume: vorticity
  ├── Slice: pressure
  │     └── Glyphs: velocity
  ├── Streamlines: velocity
  └── Probe: pressure over time

Do not force the user to choose only one active visualization mode.

E. Rendering layer

Create components or actors similar to:

- ACFDVizCaseActor
- UCFDVizVolumeComponent
- UCFDVizSliceComponent
- UCFDVizBoundaryMeshComponent
- UCFDVizGlyphComponent
- UCFDVizStreamlineComponent
- UCFDVizProbeComponent

F. UI layer

Use a desktop-oriented UMG/Slate workspace. Core view state should be represented by C++ view models, not embedded in widget event graphs.

======================================================================
6. CFDVIZ CASE FORMAT VERSION 1.0
======================================================================

Establish and implement a language-neutral, Unreal-independent data format named:

CFDViz Case Format 1.0

Canonical form:

CaseName.cfdviz/

A .cfdviz item is a directory, not a single opaque binary file.

Directory structure:

CaseName.cfdviz/
  manifest.json
  meshes/
    obstacle.cvm
    boundaries.cvm
  frames/
    000000/
      U.cvf
      pressure.cvf
      speed.cvf
      vorticity.cvf
      qcriterion.cvf
      validMask.cvf
    000001/
      ...
  optional/
    thumbnails/
    provenance/
    attachments/

A packed .cfdvizpkg ZIP container may be added later. Do not make ZIP packaging a requirement for the first release.

General format rules:

- UTF-8 JSON
- Little-endian binary
- Explicit semantic version
- Explicit units
- Explicit coordinate system
- Explicit field centering
- Explicit numeric type
- Explicit compression
- Explicit field statistics
- CRC validation
- Unknown JSON properties must be ignored when safe.
- An unsupported major version must be rejected.
- A newer minor version may be accepted when all required constructs are supported.
- Paths in manifest.json are relative to the case root.
- Prevent path traversal outside the case root.

----------------------------------------------------------------------
6.1 Canonical physical coordinates
----------------------------------------------------------------------

The canonical CFDViz physical coordinate system is:

- Right-handed
- Z-up
- X-forward
- Physical length unit specified in the manifest
- Prefer SI meters

A source case may declare a transform into canonical coordinates.

Unreal’s coordinate conversion must occur in one adapter:

- Convert physical length to Unreal centimeters.
- Apply handedness and basis conversion.
- Transform vectors with the linear basis.
- Transform normals with the inverse-transpose where needed.
- Transform second-order tensors correctly when FEA support is added.
- Never mix Unreal centimeters into stored scientific fields.

----------------------------------------------------------------------
6.2 Required manifest.json structure
----------------------------------------------------------------------

Write a JSON Schema and a complete specification. The implemented manifest must follow this conceptual example:

{
  "format": "CFDViz",
  "version": "1.0.0",

  "case": {
    "id": "d7f46da7-6bd8-4d4b-9410-32d1ea776328",
    "name": "Mock Cylinder Wake",
    "description": "Synthetic visualization demonstration; not validation-grade CFD",
    "createdUtc": "2026-08-04T00:00:00Z",
    "quality": "visualization-demo",
    "solver": {
      "name": "FlowViz Mock Wake Generator",
      "version": "1.0.0",
      "method": "analytic-vortex-street"
    },
    "tags": ["demo", "cfd", "wake", "cylinder"]
  },

  "units": {
    "length": "m",
    "time": "s",
    "mass": "kg",
    "temperature": "K",
    "angle": "rad"
  },

  "coordinates": {
    "handedness": "right",
    "upAxis": "Z",
    "forwardAxis": "X",
    "origin": [0.0, 0.0, 0.0],
    "sourceToCanonical": [
      1.0, 0.0, 0.0, 0.0,
      0.0, 1.0, 0.0, 0.0,
      0.0, 0.0, 1.0, 0.0,
      0.0, 0.0, 0.0, 1.0
    ],
    "crs": null
  },

  "timeline": {
    "frameCount": 90,
    "times": [0.0, 0.02, 0.04],
    "steps": [0, 20, 40],
    "defaultInterpolation": "linear"
  },

  "grids": [
    {
      "id": "fluid",
      "type": "uniform-cartesian",
      "dimensions": [128, 64, 24],
      "origin": [-4.0, -2.0, -0.5],
      "spacing": [0.09375, 0.0625, 0.0416666667],
      "maskField": "validMask"
    }
  ],

  "meshes": [
    {
      "id": "obstacle",
      "name": "Cylinder",
      "path": "meshes/obstacle.cvm",
      "role": "boundary",
      "static": true,
      "patches": [
        {
          "id": 1,
          "name": "cylinderWall",
          "type": "wall"
        }
      ]
    }
  ],

  "fields": [
    {
      "numericId": 1,
      "id": "U",
      "name": "Velocity",
      "semantic": "velocity",
      "kind": "vector",
      "components": ["x", "y", "z"],
      "componentCount": 3,
      "dataType": "float32",
      "association": "cell",
      "grid": "fluid",
      "unit": "m/s",
      "temporalInterpolation": "linear",

      "storage": {
        "type": "bricked-volume",
        "brickSize": [32, 32, 32],
        "codec": "zstd",
        "pathPattern": "frames/{frame:06d}/U.cvf"
      },

      "statistics": {
        "globalComponentMin": [-1.0, -1.0, -0.2],
        "globalComponentMax": [2.0, 1.0, 0.2],
        "globalMagnitudeMin": 0.0,
        "globalMagnitudeMax": 2.2
      },

      "display": {
        "defaultComponent": "magnitude",
        "defaultColorMap": "Viridis",
        "defaultRangeMode": "global",
        "recommendedRange": [0.0, 2.0],
        "opacityPoints": [
          [0.0, 0.0],
          [0.2, 0.0],
          [1.0, 0.35],
          [2.0, 0.85]
        ]
      }
    }
  ],

  "derivedFields": [
    {
      "id": "velocityMagnitude",
      "name": "Velocity Magnitude",
      "expression": "mag(U)",
      "unit": "m/s"
    }
  ],

  "structures": [],

  "provenance": {
    "generatorCommand": "python -m cfdviz generate-mock ...",
    "notes": [
      "This dataset is intended to exercise visualization controls."
    ]
  }
}

The final example must include complete times and field entries, not the abbreviated example above.

Field associations for version 1:

- cell
- point

Reserve these for future versions:

- mesh-vertex
- mesh-element
- integration-point
- face
- particle

Version 1 volume field data types:

- float16
- float32
- uint8

Do not support float64 GPU rendering in version 1. The manifest may note original solver precision separately.

----------------------------------------------------------------------
6.3 CVF bricked volume field format
----------------------------------------------------------------------

Each field at each stored frame is one .cvf file.

The file is independently readable and contains a brick directory followed by compressed brick payloads. This enables random brick reads, future region-of-interest loading, and empty-space skipping.

Use manual serialization. Do not serialize a native C++ struct with compiler-dependent padding.

CVF header:

- Fixed 128 bytes
- Little endian

Layout:

Offset  Size  Type       Name
0       8     char[8]    magic = "CFDVOL1\0"
8       4     uint32     headerBytes = 128
12      2     uint16     majorVersion = 1
14      2     uint16     minorVersion = 0
16      4     uint32     endianMarker = 0x01020304
20      4     uint32     flags
24      4     uint32     frameIndex
28      4     uint32     fieldNumericId
32      8     float64    simulationTime
40      4     uint32     dimensionX
44      4     uint32     dimensionY
48      4     uint32     dimensionZ
52      2     uint16     brickSizeX
54      2     uint16     brickSizeY
56      2     uint16     brickSizeZ
58      1     uint8      componentCount
59      1     uint8      dataType
60      1     uint8      association
61      1     uint8      codec
62      2     uint16     reserved
64      8     uint64     brickCount
72      8     uint64     directoryOffset
80      8     uint64     payloadOffset
88      16    float32[4] backgroundValue
104     4     uint32     headerCrc32c
108     20    byte[20]   reserved

Enum values:

dataType:
  1 = float16
  2 = float32
  3 = uint8

association:
  0 = cell
  1 = point

codec:
  0 = none
  1 = zstd

The header CRC is CRC-32C computed with headerCrc32c set to zero.

Each brick directory entry is 80 bytes:

Offset  Size  Type       Name
0       4     uint32     brickIndexX
4       4     uint32     brickIndexY
8       4     uint32     brickIndexZ
12      2     uint16     validSizeX
14      2     uint16     validSizeY
16      2     uint16     validSizeZ
18      2     uint16     flags
20      8     uint64     absolutePayloadOffset
28      4     uint32     compressedBytes
32      4     uint32     uncompressedBytes
36      16    float32[4] componentMin
52      16    float32[4] componentMax
68      4     uint32     payloadCrc32c
72      8     byte[8]    reserved

Payload rules:

- X index varies fastest, followed by Y, then Z.
- Components are interleaved per voxel.
- Edge bricks use validSizeX/Y/Z.
- float16 uses IEEE 754 binary16.
- Missing bricks evaluate to backgroundValue.
- A brick containing only background values may be omitted.
- NaNs are allowed in floating-point payloads.
- Statistics ignore NaNs and invalid masked cells.
- Zstd level 3 is the default.
- The uncompressed size must exactly match valid voxel count × component count × data-type size.
- Validate every offset and size before reading.
- Validate the frame index, time, field ID, and dimensions against manifest.json.
- A corrupted brick may be marked unavailable without discarding the entire case when safe.

Implement:

- Python CVF writer
- Python CVF reader
- Unreal C++ CVF reader
- Round-trip tests
- Corruption tests
- Endianness rejection test
- CRC test
- Partial-brick test
- Empty-brick test

----------------------------------------------------------------------
6.4 CVM boundary/structure mesh format
----------------------------------------------------------------------

Implement a minimal static triangle-mesh format named CVM.

Header:

- Fixed 96 bytes
- Little endian
- Manual serialization

Layout:

Offset  Size  Type       Name
0       8     char[8]    magic = "CFDMESH1"
8       2     uint16     majorVersion = 1
10      2     uint16     minorVersion = 0
12      4     uint32     endianMarker = 0x01020304
16      4     uint32     flags
20      4     uint32     headerBytes = 96
24      8     uint64     vertexCount
32      8     uint64     triangleCount
40      8     uint64     positionsOffset
48      8     uint64     normalsOffset
56      8     uint64     indicesOffset
64      8     uint64     patchIdsOffset
72      8     uint64     nodeIdsOffset
80      4     uint32     headerCrc32c
84      12    byte[12]   reserved

Flags:

- bit 0: normals present
- bit 1: patch IDs present
- bit 2: node IDs present
- bit 3: positions are float64; otherwise float32

Arrays:

- positions: XYZ per vertex
- normals: XYZ float32 per vertex when present
- triangle indices: three uint32 values per triangle
- patch IDs: one uint32 per triangle when present
- node IDs: one uint64 per vertex when present

The sample case must include named patches:

- inlet
- outlet
- sideWalls
- cylinderWall

The UI must allow per-patch visibility, color, opacity, edge display, and normal display.

----------------------------------------------------------------------
6.5 Future FEA-compatible array format
----------------------------------------------------------------------

Design and document a CVA array format for future mesh-associated data. Implement the parser and round-trip test even if the first UI does not render all FEA result types.

CVA must support:

- Mesh-vertex displacement vectors
- Mesh-vertex scalar results
- Mesh-element scalar results
- Six-component symmetric tensors
- Nine-component full tensors
- Frame index and physical time
- float16, float32, and float64 storage
- Optional compression
- Global and per-frame statistics

Add a “structures” manifest section that can associate:

- A CVM mesh
- A displacement field
- A velocity field
- Stress and strain fields
- Raw versus smoothed result variants
- Vertex-to-node mappings

Do not implement a full FEA solver.

Create interfaces for a future GPU deformation component using a static base mesh plus per-frame displacement buffers.

======================================================================
7. REFERENCE WRITER AND MOCK CFD CASE
======================================================================

Create a Python package named cfdviz under Tools/cfdviz.

Required commands:

python -m cfdviz generate-mock --output <path>
python -m cfdviz validate <case>
python -m cfdviz inspect <case>
python -m cfdviz extract <case> --frame N --field U
python -m cfdviz benchmark-read <case>

Use NumPy and zstandard. Pin compatible dependency ranges in pyproject.toml.

The required default mock case is a synthetic unsteady wake behind a cylinder.

It must be deterministic and visually useful, but it must be labeled clearly as:

“Synthetic visualization demonstration; not validation-grade CFD.”

Default case:

- 128 × 64 × 24 cells
- 90 stored frames
- Physical time step between stored frames: 0.02 seconds
- Domain approximately 12 m × 4 m × 1 m
- Cylinder centered near the upstream third
- Uniform incoming flow
- Alternating downstream vortices
- Mild spanwise variation so the volume is not perfectly extruded
- Static cylinder mesh and named domain boundary patches

Generate the following fields:

- validMask, uint8 scalar
- U, float32 three-component velocity
- pressure, float32 scalar
- speed, float32 scalar
- vorticity, float32 three-component vector
- vorticityMagnitude, float32 scalar
- qCriterion, float32 scalar
- passiveScalar, float32 scalar

Use a stable analytic construction such as a uniform base flow plus convecting, alternating Lamb–Oseen vortices. Mask the cylinder interior. Pressure may use a clearly documented synthetic relationship based on local kinetic-energy variation.

Compute gradient-derived fields consistently using finite differences in physical coordinates.

The generator must expose:

- Grid resolution
- Frame count
- Stored-frame time interval
- Domain dimensions
- Inlet velocity
- Cylinder radius
- Vortex shedding frequency
- Vortex circulation
- Vortex core radius
- Spanwise perturbation
- Compression
- float16 or float32 output where supported
- Random seed, even if the default is deterministic

Also add an optional D2Q9 cylinder-wake generator as a secondary example if it can be implemented without destabilizing the primary deliverable. The analytic generator is release-blocking; D2Q9 is not.

Include automated checks that:

- Velocity, pressure, and derived fields contain finite values outside the obstacle.
- The obstacle is masked.
- Global statistics match decoded data.
- All frames have consistent dimensions.
- Times are monotonic.
- The Unreal reader obtains the same known sample values as the Python reader.

Include a low-resolution pre-generated sample small enough for ordinary source control, or automatically generate it as part of project setup and copy it into the packaged build.

The Unreal demo map must auto-load this case on first launch.

======================================================================
8. TIME AND FRAME PLAYBACK
======================================================================

Implement a physical simulation clock independent of Unreal frame rate.

Required state:

- Current physical time
- Current stored frame A
- Next stored frame B
- Interpolation alpha
- Playback speed
- Playback mode
- Loop state
- Ping-pong state
- Frame-dropping policy
- Requested fields
- Cache state

Required playback modes:

1. Sequence:
   Display every stored frame in order.

2. Real time:
   Respect physical simulation time and skip display frames when necessary.

3. Fixed output FPS:
   Sample the simulation at a selected output frame rate.

Controls:

- Play
- Pause
- Stop
- First frame
- Last frame
- Previous stored frame
- Next stored frame
- Editable physical time
- Frame number
- Timeline slider
- Playback speed presets
- Custom playback speed
- Loop
- Ping-pong
- Interpolation on/off
- Sequence/real-time/fixed-FPS selector

GPU interpolation:

- Maintain frame A and frame B textures.
- Interpolate scalar and vector samples in the shader.
- Do not create an interpolated CPU volume merely for display.
- Use nearest-frame behavior for masks and topology.
- Display frame A, frame B, alpha, and physical time in the diagnostics panel.
- Show an “Interpolated” indicator when alpha is neither zero nor one.

Caching:

- LRU cache with configurable CPU and GPU memory budgets
- Preload at least one frame ahead and one behind
- Cancel obsolete requests during aggressive scrubbing
- Prioritize current frame over preload
- Hold the last complete frame while the new frame loads
- Never display partially updated field components as a complete frame

======================================================================
9. GPU VOLUME DATA PATH
======================================================================

Implement a reliable runtime volume path based on dynamic 3D textures or equivalent RHI resources.

Preferred design:

- Persistent RHI 3D textures
- Render-thread uploads
- Double or triple buffering
- Version-isolated wrapper around texture creation and update APIs
- Shader parameters representing grid origin, physical size, spacing, dimensions, and coordinate transform
- Separate texture resources for scalar and vector fields as needed
- RGBA texture layout where practical
- Mask texture
- Frame A and frame B resources

Do not make imported Unreal assets the only way to load data.

Do not require VDB conversion for ordinary runtime playback.

Implement a custom scientific volume renderer using either:

- A custom scene proxy/global shader, or
- A carefully designed proxy-volume material driven by dynamic 3D texture resources

The renderer must support:

- Front-to-back alpha compositing
- Maximum-intensity projection
- Minimum-intensity projection
- Average projection
- Ray-marched iso-surface mode
- Configurable sample step in voxel units
- Maximum step count
- Early ray termination
- Optional jitter to reduce banding
- Central-difference gradients
- Optional gradient lighting
- Opacity multiplier
- Transfer-function lookup texture
- Crop box
- Multiple clipping planes
- Invalid-mask rejection
- Under-range, over-range, NaN, and masked-value handling
- Anisotropic voxel spacing
- Perspective and orthographic cameras

Provide a “Scientific” rendering profile:

- Neutral lighting
- No depth of field
- No motion blur
- Neutral exposure
- Accurate pseudocolor
- Visible legend and units
- Minimal post-processing

Provide a separate “Presentation” profile:

- Contextual lighting
- Environmental geometry
- Optional shadows
- Optional depth of field
- Cinematic cameras
- Optional photorealistic smoke-like styling

Never make the Presentation profile the default for quantitative work.

Optional Unreal SVT path:

- Add an optional offline converter for selected fields to VDB/Sparse Volume Texture.
- Treat it as a separate presentation renderer.
- Do not make it the only runtime renderer.
- Keep it behind a feature flag.
- Document the version and platform limitations encountered.

======================================================================
10. REQUIRED VISUALIZATION PIPELINE FEATURES
======================================================================

The feature organization should resemble a ParaView/OpenFOAM post-processing workflow while remaining usable by someone who is not a visualization specialist.

----------------------------------------------------------------------
10.1 Case and field browser
----------------------------------------------------------------------

Display:

- Case name
- Case description
- Solver/generator provenance
- Quality classification
- Coordinate system
- Physical dimensions
- Grid dimensions
- Cell count
- Timeline range
- Stored frame count
- Available scalar fields
- Available vector fields
- Units
- Centering
- Data type
- Global statistics
- Meshes
- Boundary patches

Allow:

- Search/filter
- Friendly names versus raw field IDs
- Metadata inspection
- Copy metadata
- Validation report
- Reload case
- Close case

----------------------------------------------------------------------
10.2 Representations
----------------------------------------------------------------------

Support these representation concepts:

- Volume
- Surface
- Surface with edges
- Wireframe
- Points
- Slice
- Iso-surface
- Vector glyphs
- Streamlines
- Particles/pathlines

Not every representation applies to every data source. Disable invalid choices with a useful explanation.

----------------------------------------------------------------------
10.3 Scalar coloring and transfer functions
----------------------------------------------------------------------

Required controls:

- Select scalar field
- Select vector component X/Y/Z or magnitude
- Select tensor component in future
- Solid color
- Continuous color map
- Discrete color bands
- Number of bands
- Reverse color map
- Clamp to range
- Under-range color
- Over-range color
- NaN color
- Masked color
- Linear scale
- Log scale
- Symmetric-log scale if practical
- Global case range
- Current-frame range
- Manual range
- Percentile range
- Reset range
- Histogram
- Editable color control points
- Editable opacity control points
- Preset save/load
- Legend visibility
- Legend orientation
- Legend title
- Unit label
- Numeric format
- Significant digits
- Tick count
- Legend position

Ship these initial color maps:

- Viridis
- Plasma
- Inferno
- Magma
- Turbo
- CoolWarm
- Blue–White–Red
- Grayscale

Use a perceptually reasonable map such as Viridis as the default, not a rainbow map.

----------------------------------------------------------------------
10.4 Slices
----------------------------------------------------------------------

Support arbitrary slice planes.

Controls:

- Field
- Origin XYZ
- Normal XYZ
- X/Y/Z presets
- Center-on-domain
- Plane translation gizmo
- Plane rotation gizmo
- Show/hide plane widget
- Thickness
- Number of slab samples
- Slab operation: average, minimum, maximum
- Opacity
- Color map
- Contour-line overlay
- Contour count
- Manual contour values
- Vector-glyph overlay
- Sample resolution
- Nearest or trilinear interpolation

The slice must update interactively while dragging, with a reduced-quality drag mode if needed.

----------------------------------------------------------------------
10.5 Clipping and cropping
----------------------------------------------------------------------

Support:

- Plane clip
- Axis-aligned box crop
- Oriented box crop
- Sphere crop as an advanced feature
- Multiple simultaneous planes
- Invert
- Keep inside/outside
- Numeric transform
- Interactive transform gizmo
- Reset to bounds
- Show/hide clipping widgets

Clipping must apply consistently to volume rendering, slices, glyphs, and streamlines where feasible.

----------------------------------------------------------------------
10.6 Iso-surfaces and contours
----------------------------------------------------------------------

Release-blocking implementation:

- Ray-marched scalar iso-surface
- One or multiple iso values
- Automatic equally spaced values
- Manual values
- Surface opacity
- Surface color
- Color iso-surface by the same or another scalar field
- Gradient-based normal
- Crop and clipping support

Optional enhanced implementation:

- Asynchronous CPU or GPU marching cubes
- Generated triangle mesh
- Smoothing
- Decimation
- Mesh export

Do not block the initial release on mesh-producing marching cubes if the ray-marched iso-surface works correctly.

----------------------------------------------------------------------
10.7 Vector glyphs
----------------------------------------------------------------------

Required:

- Vector field selection
- Arrow glyph
- Line glyph
- Cone glyph if practical
- Regular-grid seeding
- Slice-plane seeding
- Box seeding
- Glyph count/density
- Scale by vector magnitude
- Constant scale
- Clamp scale
- Normalize vectors
- Color by magnitude or another scalar
- Minimum-magnitude threshold
- Maximum glyph count safety limit
- Instanced rendering

Use an instanced mesh path rather than generating a unique mesh for every glyph.

----------------------------------------------------------------------
10.8 Streamlines
----------------------------------------------------------------------

Implement streamlines for the current time-varying velocity snapshot.

Required seed types:

- Point
- Line
- Plane
- Box
- Sphere

Required integration controls:

- Forward
- Backward
- Both directions
- RK4 integration
- Physical step length
- Maximum steps
- Maximum physical length
- Minimum velocity cutoff
- Domain exit
- Mask/solid collision termination
- Seed count
- Seed randomization with fixed seed
- Tube or ribbon display
- Line/tube radius
- Color by speed
- Color by another scalar field
- Opacity
- Show direction markers

Run integration asynchronously. Debounce settings while sliders are moving. Cancel stale integration jobs.

For the low-resolution demo, support at least 128 simultaneous streamlines interactively.

----------------------------------------------------------------------
10.9 Animated particles and pathlines
----------------------------------------------------------------------

Use Niagara for animated particles where practical.

Drive Niagara from the imported velocity texture rather than from a Niagara fluid simulation.

Controls:

- Seed source
- Release rate
- Burst count
- Lifetime
- Particle size
- Velocity scale
- Trail length
- Color field
- Opacity
- Reset when timeline scrubs
- Follow physical time or display time
- Integration substeps

Clearly distinguish:

- Streamlines: instantaneous vector field
- Pathlines: trajectories through a time-varying field
- Streaklines: particles released over time from a source

Full temporally accurate pathlines may be a second milestone, but the architecture must not confuse them with instantaneous streamlines.

----------------------------------------------------------------------
10.10 Thresholding
----------------------------------------------------------------------

Support scalar thresholding:

- Field
- Minimum
- Maximum
- Inside/outside
- Apply to volume opacity
- Apply to glyph visibility
- Apply to streamline termination
- Histogram-linked handles

----------------------------------------------------------------------
10.11 Probes and quantitative inspection
----------------------------------------------------------------------

Point probe:

- Place by numeric XYZ
- Place by clicking a slice
- Place on boundary geometry
- Drag probe
- Display current values
- Select fields
- Display vector components and magnitude
- Pin multiple probes
- Rename probes
- Show coordinate and units
- Plot selected values over time
- Export CSV

Line probe:

- Two endpoints
- Interactive handles
- Number of samples
- Distance or normalized-distance X axis
- Multiple scalar series
- Current-time plot
- Export CSV

Statistics:

- Field minimum and maximum
- Locations of minimum and maximum
- Mean
- RMS where applicable
- Standard deviation
- Histogram
- Valid sample count
- Invalid sample count

Surface and volume integration may be milestone two, but reserve pipeline nodes for:

- Area average
- Volume average
- Surface integral
- Flux
- Force
- Moment
- Mass flow

Never infer force or flux unless the required pressure, velocity, density, viscosity, normals, and region information are actually available.

----------------------------------------------------------------------
10.12 Derived fields and calculator
----------------------------------------------------------------------

Implement built-in derived quantities for a uniform Cartesian grid:

- Vector magnitude
- Velocity divergence
- Vorticity/curl
- Vorticity magnitude
- Q-criterion
- Strain-rate magnitude
- Pressure coefficient when reference values are supplied
- Scalar gradient magnitude

Implement a small expression calculator supporting:

- Numeric constants
- Scalar field references
- Vector component access such as U.x
- Addition
- Subtraction
- Multiplication
- Division
- Parentheses
- mag(vector)
- dot(vector, vector)
- min
- max
- abs
- sqrt

It is acceptable for the first calculator to compute only the currently selected frame asynchronously.

Requirements:

- Parse errors shown in UI
- Unit field entered or inferred when safe
- No silent unit assumptions
- Computed field can be added to the pipeline
- Computed field settings persist in sessions
- Cache computed results

----------------------------------------------------------------------
10.13 Boundary and geometry controls
----------------------------------------------------------------------

For every boundary patch:

- Visibility
- Color
- Opacity
- Surface
- Surface with edges
- Wireframe
- Normal display
- Patch name labels
- Select all/none
- Search
- Solo patch
- Reset display

Global geometry controls:

- Show domain bounds
- Show grid outline
- Show axes triad
- Show scale bar
- Show origin
- Show cell spacing
- Show physical dimensions
- Fit camera to case
- Transparent obstacle mode
- Clipping interaction

======================================================================
11. USER INTERFACE
======================================================================

Create a runtime scientific-visualization workspace with this default layout:

Top:
- Main menu
- Visualization toolbar

Left:
- Case browser
- Visualization pipeline tree
- Boundary patch tree

Center:
- Main 3D viewport

Right:
- Properties tab
- Display tab
- Color/opacity editor tab
- Metadata tab

Bottom:
- Timeline
- Probe/chart area
- Status bar

Panels must be resizable and collapsible.

Main menu:

File
- Open Case
- Open Recent
- Close Case
- Save Session
- Save Session As
- Load Session
- Export Screenshot
- Export Data
- Exit

Edit
- Undo
- Redo
- Delete Node
- Duplicate Node
- Reset Selected Node

View
- Scientific Profile
- Presentation Profile
- Perspective
- Orthographic
- Standard camera views
- Fit to Data
- Axes
- Scale Bar
- Grid Bounds
- Diagnostics
- Full Screen

Filters
- Volume
- Slice
- Clip
- Iso-surface
- Threshold
- Glyphs
- Streamlines
- Particles
- Point Probe
- Line Probe
- Histogram
- Calculator

Tools
- Validate Case
- Case Inspector
- Performance Monitor
- Generate Mock Case
- Color Map Manager
- Camera Bookmarks

Help
- Controls
- Data Format
- About

Toolbar:

- Open
- Save session
- Undo
- Redo
- Reset camera
- Camera presets
- Add slice
- Add clip
- Add iso-surface
- Add glyphs
- Add streamlines
- Add probe
- Screenshot
- Presentation mode

Properties:

- Searchable
- Basic/Advanced mode
- Numeric entry plus slider where appropriate
- Unit-aware labels
- Reset button per property
- Useful tooltip
- Apply button for expensive operations
- Optional Auto Apply
- Visible recompute progress
- Cancel operation

Pipeline tree interactions:

- Visibility eye
- Selection
- Rename
- Delete
- Duplicate
- Drag reorder where meaningful
- Parent/input relationship
- Error/warning icon
- Dirty/recomputing indicator
- Context menu
- Solo node
- Hide all others

Undo/redo:

Implement a command stack for:

- Adding/removing nodes
- Property changes
- Camera bookmarks
- Probe placement
- Transfer-function edits

Coalesce continuous slider drags into one undo command.

======================================================================
12. CAMERA AND VIEWPORT INTERACTION
======================================================================

Implement:

- Orbit
- Pan
- Dolly
- Fly
- Focus selected
- Fit all data
- Fit selected patch
- Perspective
- Orthographic
- Front/back/left/right/top/bottom
- View cube or equivalent orientation widget
- XYZ axes triad
- Camera clipping controls
- Camera speed
- Camera bookmarks
- Copy/paste camera transform

Default scientific navigation should feel familiar to users of engineering and DCC applications.

Show the cursor’s physical coordinates when interacting with a slice or geometry.

Support a ruler tool:

- Two points
- Total distance
- Delta X/Y/Z
- Physical units
- Draggable endpoints

======================================================================
13. CHARTS
======================================================================

Implement a lightweight native Slate/UMG chart widget rather than relying on a commercial plugin.

Required:

- Multiple series
- Line chart
- Histogram
- Axis labels
- Units
- Legend
- Pan
- Zoom
- Reset axes
- Automatic range
- Manual range
- Linear/log axis where valid
- Cursor readout
- CSV export

Use data decimation for very large temporal series while retaining exact values for export.

Required initial chart use cases:

- Point probe over time
- Line probe at current time
- Field histogram
- Load/decompression/upload timing diagnostics

======================================================================
14. SESSION AND PRESET FORMAT
======================================================================

Create a JSON session format:

- .cfdvizsession

It must store:

- Format/version
- Case URI
- Current physical time
- Playback settings
- Visualization pipeline nodes
- Node UUIDs and parent UUIDs
- Field selections
- Transfer functions
- Color ranges
- Clip and slice transforms
- Probe positions
- Camera
- Camera bookmarks
- Boundary patch display state
- Scientific/presentation mode
- UI panel visibility
- Annotations

Use relative case paths when possible.

Loading a session with a missing case must allow the user to relink the case.

Create named visualization presets:

- Pressure Slice
- Velocity Glyphs
- Vorticity Volume
- Q-Criterion Iso-Surface
- Cylinder Wake Streamlines
- Presentation Wake

======================================================================
15. EXPORT
======================================================================

Required:

- PNG screenshot
- Optional transparent background
- Resolution multiplier
- Include/exclude UI
- Include/exclude legend
- Include/exclude title block
- CSV export for probes and charts
- Session export

Title block options:

- Case name
- Physical time
- Frame
- Field
- Units
- Color range
- “Interpolated” state
- Data-quality label
- Solver/generator provenance

Add an Unreal cinematic integration path:

- Expose physical simulation time as an interpolatable Unreal property.
- Allow Sequencer to keyframe the visualizer’s simulation time.
- Allow camera animation.
- Document Movie Render Queue usage.
- Provide a sample Level Sequence or a command that creates one.

Do not block the basic application on automated Movie Render Queue execution in packaged builds.

======================================================================
16. UNREAL-SPECIFIC DIFFERENTIATORS
======================================================================

After the scientific MVP is working, implement or scaffold these differentiators in this priority order.

----------------------------------------------------------------------
16.1 Scientific mode versus cinematic mode
----------------------------------------------------------------------

Provide an explicit mode switch.

Scientific mode optimizes for trustworthy reading of fields.

Cinematic mode allows:

- Environmental lighting
- PBR boundary materials
- Context geometry
- Camera paths
- Depth of field
- Motion blur
- Volumetric-style appearance
- Presentation annotations
- High-resolution output

The UI must never make a cinematic effect appear to be additional solver information.

----------------------------------------------------------------------
16.2 Visualization stories
----------------------------------------------------------------------

Create a “Story” object containing ordered shots.

Each shot can capture:

- Camera
- Physical time
- Playback speed
- Visible pipeline nodes
- Transfer functions
- Clip/slice state
- Annotations
- Duration
- Transition

Allow:

- Add Current View as Shot
- Reorder
- Preview
- Generate a Sequencer sequence
- Export screenshots for each shot

This turns a technical inspection into a repeatable presentation without rebuilding the visualization.

----------------------------------------------------------------------
16.3 Immersive OpenXR inspection
----------------------------------------------------------------------

Scaffold an OpenXR mode.

Desired interactions:

- View case at miniature scale or 1:1 scale
- Grab and move slice planes
- Grab and rotate clipping planes
- Place probes
- Move seed regions
- Measure distances
- Change world scale
- Teleport or fly through the field
- Reset view

Keep all scientific state shared with desktop mode.

OpenXR is not release-blocking for version 0.1, but the viewport and interaction architecture must not make it impossible.

----------------------------------------------------------------------
16.4 Pixel Streaming
----------------------------------------------------------------------

Ensure the packaged application and UI can later operate through Unreal Pixel Streaming.

Avoid interactions that require inaccessible native dialogs with no alternative.

Provide:

- Command-line case path
- Recent-case list
- In-application path entry
- Keyboard-accessible UI
- Resolution-independent layout

Document a Pixel Streaming deployment path, but do not make cloud infrastructure a version 0.1 requirement.

----------------------------------------------------------------------
16.5 Georeferenced cases
----------------------------------------------------------------------

Reserve optional manifest metadata for:

- CRS identifier
- Latitude/longitude/height origin
- Local East/North/Up basis
- Projected coordinate system
- Earth-centered coordinates

Create an adapter interface for Unreal’s georeferencing system.

This is especially valuable for atmospheric, ocean, river, terrain, and vehicle-flow simulations.

Do not make georeferencing a dependency for ordinary local CFD cases.

----------------------------------------------------------------------
16.6 Case comparison
----------------------------------------------------------------------

Design for a future synchronized comparison mode:

- Side by side
- Overlay
- Difference field
- Relative difference
- Synchronized cameras
- Synchronized physical time
- Synchronized slice and clip transforms
- Independent or shared color ranges

Add the data-model interfaces and a backlog item. Implement side-by-side synchronized views if the current project schedule permits after the required MVP is complete.

----------------------------------------------------------------------
16.7 Live solver streaming
----------------------------------------------------------------------

Design the data-source API so a future live source can:

- Send the manifest once
- Send CVF brick payloads incrementally
- Mark frames complete
- Replace or refine bricks
- Advertise available fields
- Report solver progress
- Drop old frames
- Pause/resume
- Support region-of-interest priority

Document a possible message protocol using the existing CVF header and brick entries.

Do not implement a production network protocol in version 0.1.

======================================================================
17. PERFORMANCE AND DIAGNOSTICS
======================================================================

Add a diagnostics overlay showing:

- Unreal FPS
- Game-thread time
- Render-thread time
- GPU time if available
- Current frame
- Current time
- Interpolation alpha
- Loaded fields
- CPU cache use
- GPU cache use
- Cache hit rate
- Pending I/O requests
- Pending decompression
- Disk read time
- Decompression time
- GPU upload time
- Streamline computation time
- Volume sample count
- Volume ray step
- Current resolution
- Current clipping state

Add Unreal trace events and a log category:

- LogFlowViz

Add console commands where useful:

- FlowViz.LoadCase
- FlowViz.ReloadCase
- FlowViz.ClearCache
- FlowViz.ShowDiagnostics
- FlowViz.SetCpuCacheMB
- FlowViz.SetGpuCacheMB
- FlowViz.DumpCase
- FlowViz.Benchmark

Performance targets for the included sample are goals, not reasons to fake results:

- Responsive timeline scrubbing
- No synchronous disk loading on the game thread
- No full texture-resource recreation per frame
- At least 30 FPS with the sample volume plus a moderate streamline count on a reasonably modern desktop GPU
- UI remains responsive while expensive filters recompute
- Memory use obeys configured cache budgets

Record actual measured hardware and results in PERFORMANCE.md. Do not claim performance that was not measured.

Future optimization hooks:

- Brick occupancy/min-max texture
- Empty-space skipping
- Mip levels
- Region-of-interest loading
- Frustum-prioritized loading
- Reduced quality during interaction
- Higher quality when stationary
- GPU streamline integration
- GPU marching cubes
- Asynchronous compute

======================================================================
18. TESTING
======================================================================

Python tests:

- Manifest schema validation
- CVF round trip
- CVM round trip
- CVA round trip
- CRC failure
- Truncated file
- Bad offset
- Unsupported major version
- Partial edge brick
- Omitted background brick
- NaN preservation
- float16 and float32
- Known sample values
- Global statistics
- Deterministic mock generation

Unreal automation tests:

- Manifest parse
- Missing required property
- Coordinate conversion
- Vector basis conversion
- Grid cell-center calculation
- Trilinear scalar sampling
- Trilinear vector sampling
- Mask sampling
- Temporal interpolation
- CVF decode
- CRC rejection
- Cache insertion and eviction
- Request cancellation
- Session round trip
- Pipeline serialization
- Mock case load
- Boundary mesh load

Functional smoke test:

1. Launch demo map.
2. Auto-load sample case.
3. Show velocity-magnitude volume.
4. Play timeline.
5. Pause and scrub.
6. Add pressure slice.
7. Move slice.
8. Add velocity glyphs to slice.
9. Add streamlines.
10. Add point probe.
11. Show probe chart.
12. Change color range.
13. Edit opacity.
14. Add clipping plane.
15. Save session.
16. Reload session.
17. Capture screenshot.

Add a command-line or automation path for this smoke test where possible.

======================================================================
19. OPENFOAM/PARAVIEW FEATURE PARITY DOCUMENT
======================================================================

Create Docs/OPENFOAM_PARAVIEW_PARITY.md.

The document must map each expected post-processing concept to:

- FlowViz control
- Implementation status
- Source code location
- Known limitation
- Planned milestone

Include at least:

- Pipeline browser
- Properties and display settings
- Surface
- Surface with edges
- Wireframe
- Points
- Volume
- Slice
- Clip
- Threshold
- Contour/iso-surface
- Glyph
- Stream tracer
- Particle tracer/pathlines
- Calculator
- Transform
- Warp by vector
- Probe location
- Plot over line
- Plot over time
- Histogram
- Field minimum/maximum
- Integrate variables
- Surface sampling
- Temporal interpolation
- Temporal statistics
- Color map editor
- Opacity transfer function
- Scalar legend
- Camera presets
- Animation controls
- Screenshot/movie export
- Save/load state
- Multiple views
- Boundary/block visibility

Do not claim full parity where a feature is only scaffolded.

======================================================================
20. INITIAL RELEASE SCOPE
======================================================================

Version 0.1 is complete only when these work end to end:

- CFDViz manifest and schema
- CVF reader/writer
- CVM reader/writer
- Mock case generator
- Runtime case loading
- Asynchronous decompression
- GPU volume upload
- Timeline playback
- GPU frame interpolation
- Scalar volume rendering
- Transfer-function editor
- Stable color ranges
- Scalar legend
- Arbitrary slice
- Clip plane and crop box
- Ray-marched iso-surface
- Vector glyphs
- Streamlines
- Point probe
- Line probe
- Basic charts
- Boundary patch controls
- Pipeline tree
- Properties panel
- Scientific/presentation mode
- Session save/load
- Screenshot
- Diagnostics
- Documentation
- Automated tests
- Packaged desktop build

Version 0.1 may defer:

- Full FEA deformation rendering
- Unstructured CFD grids
- AMR
- GPU marching cubes
- Fully accurate time-dependent pathlines
- OpenXR
- Pixel Streaming deployment
- Georeferencing
- Multi-case difference fields
- Direct OpenFOAM reader
- Live solver networking

Do not allow deferred work to leave broken visible controls in the version 0.1 UI.

======================================================================
21. OUT OF SCOPE
======================================================================

Do not spend version 0.1 effort on:

- A production LBM solver
- A production FEA solver
- Meshing
- Boundary-condition setup
- Solver convergence controls
- Direct editing of OpenFOAM dictionaries
- Full CAD repair
- General-purpose material authoring
- A cloud backend
- Authentication
- Database storage
- Multi-user synchronization
- Mobile builds

======================================================================
22. IMPLEMENTATION ORDER
======================================================================

Proceed in working vertical slices.

Milestone A: Format and mock data

- Write CFDVIZ_FORMAT.md
- Write JSON Schema
- Implement Python writer/reader
- Generate sample
- Implement validators and tests

Milestone B: Unreal ingestion

- Parse manifest
- Decode CVF and CVM
- Load asynchronously
- CPU field sampler
- Timeline and frame cache
- Tests

Milestone C: First visible result

- Upload scalar field
- Render a volume
- Animate frames
- Scrub timeline
- Show legend and physical time

Milestone D: Scientific interaction

- Transfer-function editor
- Slice
- Clip/crop
- Iso-surface
- Boundary patches
- Camera controls

Milestone E: Flow inspection

- Glyphs
- Streamlines
- Particles
- Point probe
- Line probe
- Charts
- Statistics

Milestone F: Productization

- Pipeline UI
- Session save/load
- Screenshot/export
- Diagnostics
- Scientific/presentation profiles
- Packaged build
- Documentation

Keep the build working after each milestone. Do not postpone all integration until the end.

======================================================================
23. DEFINITION OF DONE
======================================================================

The work is done when:

1. The project compiles on the available Unreal Engine installation.

2. Python tests pass.

3. Unreal automation tests that can run in the environment pass.

4. The demo application opens the supplied case without manual asset importing.

5. The timeline animates the mock wake.

6. Scrubbing does not synchronously read and decompress on the game thread.

7. Color and opacity controls visibly affect the volume.

8. A pressure slice can coexist with a vorticity volume.

9. Velocity glyphs can be attached to a slice.

10. Streamlines can be seeded and adjusted.

11. A point probe displays numeric values and a time plot.

12. A line probe displays a distance plot.

13. Boundary patches can be hidden independently.

14. A session can be saved, the application restarted, and the visualization restored.

15. A screenshot can include the case name, field, time, range, and legend.

16. Corrupted input is rejected with a useful error.

17. The sample’s known values match between Python and Unreal readers.

18. README.md contains exact setup, generation, build, launch, and packaging commands.

19. The final report distinguishes tested functionality from deferred functionality.

20. There are no critical-path TODOs, placeholder controls, or fake success messages.

======================================================================
24. FINAL REPORT
======================================================================

At completion, report:

- Tested Unreal Engine version
- Host OS
- Build configuration
- Major files created
- Architecture summary
- Data-format summary
- Commands to generate and validate sample data
- Commands to build and launch
- Commands to run tests
- Packaged-build location
- Implemented visualization controls
- Performance measurements and hardware
- Known limitations
- Deferred backlog
- Any engine APIs that required compatibility work
- Any third-party dependencies and licenses

Include screenshots or captured frames when the environment supports them.

Be precise and honest. Do not describe an untested feature as working.
