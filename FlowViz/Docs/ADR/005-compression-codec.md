# ADR 005 — Compression codec for CFDViz payloads

- **Status:** Accepted
- **Date:** 2026-08-04
- **Supersedes:** the zstd mandate in `plan.md` §6 / §7
- **Affects:** `Docs/CFDVIZ_FORMAT.md` §7, `Tools/cfdviz/src/cfdviz/codecs.py`,
  the Unreal CVF reader, `Tools/cfdviz/pyproject.toml`

## Context

The plan mandates zstd as the compression codec for CVF brick payloads and CVA
arrays. zstd is the right default on general engineering grounds — it dominates
zlib on the speed/ratio curve for exactly this kind of bulk numeric data.

The problem is not zstd's merits. It is that **both** ends of this format have to
decode it: a Python reference implementation and an Unreal Engine 5.8 runtime C++
module. The format is only useful if the two agree byte-for-byte.

We checked what the installed engine actually provides rather than assuming:

```
$ find "/Users/Shared/Epic Games/UE_5.8/Engine" -iname "*zstd*"
Engine/Binaries/DotNET/AutomationTool/ZstdSharp.dll
Engine/Binaries/DotNET/UnrealBuildTool/ZstdSharp.dll
Engine/Binaries/DotNET/AutomationTool/**/net10.0/ZstdSharp.dll
Engine/Source/ThirdParty/Licenses/zstd_aec56a5.LICENSE
Engine/Source/ThirdParty/Licenses/facebook_zstd_v1.5.0.LICENSE
Engine/Source/ThirdParty/Licenses/zstd_License.txt
Engine/Source/ThirdParty/vcpkg/zstd.tps
Engine/Source/ThirdParty/DirectML/facebook_zstd_v1.5.0.tps
Engine/Source/ThirdParty/Boost/**/boost/iostreams/filter/zstd.hpp
Engine/Plugins/Experimental/PythonFoundationPackages/**/zstd_aec56a5.tps
```

Every hit is one of: a **.NET assembly** (`ZstdSharp.dll`, used by
UnrealBuildTool and AutomationTool — build-time C# tooling, not linkable from a
runtime C++ module), a **licence or `.tps` attribution file**, or a **Boost
header** that forwards to a library Boost does not ship here.

`Engine/Source/ThirdParty/` contains a `zlib` module and **no `zstd` module**.
The engine-native compression formats exposed through `FCompression` are
`NAME_Zlib`, `NAME_LZ4`, and `NAME_Oodle`
(`Engine/Source/Runtime/Core/Public/Misc/Compression.h`).

So mandating zstd leaves exactly three options:

1. Ship a format the Unreal reader cannot decode — including the sample case
   this project ships and auto-loads. Release-blocking.
2. Vendor zstd from source and add it to the build. A new third-party
   dependency, a new build step on two platforms, new licence obligations.
3. Use a codec the engine already has.

## Decision

**The shipping codec is zlib (`codec = 3`).**

- Python side: the `zlib` standard library module — no dependency at all.
- Unreal side: `FCompression::UncompressMemory(NAME_Zlib, ...)` — engine-native,
  no new third-party code.
- Payloads use the **zlib container format (RFC 1950)** — 2-byte header plus
  trailing Adler-32 — which is exactly what Python's `zlib.compress()` emits and
  what `NAME_Zlib` consumes. Raw DEFLATE (RFC 1951) is **not** used. This
  distinction is the one that silently produces garbage if got wrong, so it is
  normative in the format spec rather than left to implementers.
- Default level is **6**.

`codec = 2` (lz4) remains permitted and is decodable via `NAME_LZ4`. The Python
writer emits it only when the optional `lz4` package is installed.

`codec = 1` (zstd) stays **reserved in the enum but unimplemented**. A 1.0 reader
MUST reject it with exactly:

```
CVF codec 'zstd' is reserved but not supported in CFDViz 1.0. Re-export this case with codec 'zlib'.
```

It must not crash and must not silently fall back to another codec. Reserving the
ID rather than deleting it keeps the door open: a future 1.x reader can add zstd
without renumbering anything, and files written by that future writer will be
rejected by 1.0 readers with an actionable message instead of a corrupt decode.

`zstandard` is deliberately **not** a dependency in `pyproject.toml`. If it were,
Python could write cases Unreal could not read — the failure mode this ADR exists
to prevent.

## Consequences

**Cost.** zlib compresses slower and somewhat worse than zstd. For float32
volume data the ratio gap is real but modest, and this project's bottleneck is
GPU upload and render, not decompression — brick decode already happens off the
game thread (rule 1), so the extra milliseconds do not touch frame time. If
profiling later shows decompression on the critical path, lz4 is available today
at the cost of ratio, and Oodle is available engine-side.

**Benefit.** Zero new third-party dependencies, zero new build steps, and the
guarantee that anything the Python tool writes, the Unreal runtime can read. That
guarantee is the whole point of having a reference implementation.

**Deviation is recorded, not hidden.** `Docs/CFDVIZ_FORMAT.md` §7 states the
deviation and its justification inline, so an implementer reading only the format
spec still learns why the plan was overridden.

## Alternatives considered

**Vendor zstd into `Source/ThirdParty/`.** Rejected. Buys a better ratio in
exchange for cross-platform build maintenance, licence tracking, and an upgrade
treadmill — all for a codec whose cost is not on this project's critical path.

**Oodle (`NAME_Oodle`).** Engine-native and excellent, but Unreal-only. The
Python reference implementation could not write it, which breaks the two-reader
agreement that makes this format testable at all.

**LZ4 as the default.** Fastest to decode, but a materially worse ratio on
float32 volume data, and it requires an optional Python package. Kept as a
permitted alternative (`codec = 2`), not the default.

**Uncompressed (`codec = 0`).** Simplest, and legal in the format for
already-incompressible or tiny payloads, but unacceptable as the default for
multi-frame volume cases.

## Verification

Cross-language agreement is proven mechanically, not by inspection: the generator
writes `known_values.json` (format spec §9) recording exact IEEE-754 bit patterns
read back **through the Python reader**, and the Unreal automation test asserts
on those bit patterns. A codec mismatch between the two implementations fails
that test rather than producing subtly wrong pixels.
