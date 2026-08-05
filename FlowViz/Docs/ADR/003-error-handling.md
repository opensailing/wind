# ADR 003 — Error handling at the untrusted-input boundary

- **Status:** Accepted
- **Date:** 2026-08-04
- **Affects:** `Plugins/FlowVizRuntime/.../Public/CFDViz/CFDVizTypes.h`
  (`ECFDVizError`, `FCFDVizResult`), every reader in `Private/CFDViz/`,
  every reader test
- **Related:** [ADR 001](001-case-format.md) (the format this defends),
  [ADR 005](005-compression-codec.md) (the codec whose failures it reports)

## Context

Rules 12 and 13 of `plan.md` §4 are the two the reader layer exists to satisfy:

> 12. All package offsets, dimensions, counts, and uncompressed sizes must be
>     bounds-checked before allocation.
> 13. A malformed or partially corrupted case must produce a useful error
>     message instead of a crash.

Both are about the same boundary. A `.cvf` file is untrusted input: a `uint64`
in its header can say the brick directory is at offset 2^63, that a payload
decompresses to 40 GB, or that a 128-byte header is 4 bytes long. The reader
runs inside the Editor and inside a packaged application, where "crash" means
the user loses their session.

Unreal offers three mechanisms and they are not interchangeable:

- **`check()` / `checkf()`** — fatal, and compiled out in Shipping. Using them
  on untrusted input means a malformed file either kills the process or, in a
  packaged build, is *not checked at all*. That is the exact opposite of rule
  13.
- **`ensure()`** — non-fatal but reports to the crash reporter and is intended
  for programmer error. A user's corrupt file is not a bug in FlowViz.
- **C++ exceptions** — disabled in Unreal by default and not idiomatic in
  engine code.

None of these carries *why* a file was rejected to a UI that has to explain it,
which is the actual requirement in rule 13: "a useful error message."

## Decision

**A returned result value, never an assertion, for anything derived from file
contents.** Two types:

- `ECFDVizError` — a closed enum where each value names one specific rejection
  (`BadMagic`, `UnsupportedEndianness`, `HeaderCrcMismatch`,
  `DirectoryOutOfBounds`, `PayloadOutOfBounds`, `SizeMismatch`, …).
- `FCFDVizResult` — that enum plus an optional message, file path, and byte
  offset. Default-constructs to success, so a function that forgets to set a
  failure returns success only if it also forgot to return early.

The division of labour is strict:

| Mechanism | Used for | Example |
| --- | --- | --- |
| `static_assert` | facts about *our own* layout, provable at compile time | the ~15 assertions in `CFDVizVolumeReader.cpp` pinning every CVF header field to its spec offset |
| `FCFDVizResult` | anything read from a file | `headerBytes` disagreeing with the spec |
| `check()` | caller-supplied invariants only — never a file-derived value | `check(Source != nullptr)` |

`static_assert` is the right tool for header offsets precisely because those
are not untrusted: they are the spec transcribed into code, and getting them
wrong is a programmer error that should never reach a runtime check.

The line is drawn at *provenance*, not at severity. A verified sweep of
`Private/CFDViz/` and `Public/CFDViz/` finds exactly four `check()` call sites
and no `checkf`, `ensure`, or `throw`:

| Site | Guards |
| --- | --- |
| `CFDVizCrc32C.cpp:46` | `Data != nullptr` when `Size > 0` |
| `CFDVizVolumeReader.cpp:1078`, `:1162` | `Source != nullptr` |
| `CFDVizColorMaps.cpp:178` | the built-in colour-stop table is non-empty |

Every one of those is a fact about how FlowViz called itself — a null source
handle or an empty compiled-in table is a bug in this repository, and the
crash-on-Development / compiled-out-in-Shipping behaviour is appropriate for
exactly that. None of them can be made to fire by a malformed `.cvf`. That is
the invariant this ADR asserts: **no value that originated in a file reaches a
`check()`.**

### Bounds checks are written to survive hostile arithmetic

Rule 12 says "before allocation," which is necessary but not sufficient: the
check itself must not overflow. `CFDViz::FByteCursor::CanRead` is written

```cpp
Count <= Size - Offset      // not: Offset + Count <= Size
```

because the obvious form wraps when a header claims a 2^63 length, and a
wrapped comparison passes. Every size that reaches an allocation goes through
this cursor.

### An error is data, not a log line

`FCFDVizResult` carries the byte offset because "PayloadCrcMismatch" alone
cannot be acted on. `LogIfFailed()` exists so a caller that genuinely has
nowhere to surface an error still leaves a trace, but the value is what
propagates.

## Consequences

**Every reader call site must check a return value.** There is no ambient
mechanism that stops execution, so a caller that ignores the result proceeds
with default-constructed data. This is the cost of the choice, and the reason
readers return the result rather than writing to an out-parameter that is easy
to leave unread.

**Shipping builds behave identically to Development builds at this boundary.**
Nothing here is compiled out. A file that is rejected at a developer's desk is
rejected in the packaged application, with the same message.

**The enum is closed and will grow.** Adding a rejection reason means adding a
value, which is deliberate: a generic `ParseFailed` bucket would satisfy the
type system and defeat rule 13.

**`bValid` is not the same as "no error."** `FCFDVizStatistics` reports
`bValid = false` for a field that parsed correctly and contains no valid
samples — an all-`NaN` field is a successful read of unusable data. Conflating
the two would let a consumer build a colour range from nothing.

**The overflow-safe check is verified, not merely written.** Rule 12's
enforcement lives almost entirely in `CFDVizByteCursor.h`, which for a time had
no direct test — it was exercised only indirectly, through readers that feed it
mostly *valid* input. `CFDVizByteCursorTest.cpp` now tests it against the only
inputs that distinguish the safe formulation from the naive one: counts of
`INT64_MAX`, `INT64_MIN`, `2^62`, and a value chosen so the naive addition
wraps to exactly zero. On every ordinary input the two forms agree, so a test
without those cases would have passed against the broken implementation.

That this test can actually fail is established by mutation, not asserted:
replacing `Count <= Size - Offset` with the naive `Offset + Count <= Size` is
one of the mutants `Tools/mutate.sh` runs, and the suite catches it. One result
from that campaign is worth recording because it is easy to misread. A mutant
that laundered a float through `static_cast<float>(static_cast<double>(x) *
1.0)` appeared to survive; it does not, and the first script to score it was
broken (see `ARCHITECTURE.md`). But the multiply-by-one form is also an
*equivalent mutant* — at `-O1` and above the compiler folds it away entirely,
so it changes no behaviour and proves nothing either way. Laundering through a
`volatile double`, which the optimizer may not remove, does change the value:
a signalling NaN payload `0x7F812345` comes back quieted to `0x7FC12345`. Re-run
under `Tools/mutate.sh` in an isolated worktree, that mutant comes back
`killed`, which is the evidence that the bit-pattern assertion is real. **A
mutant the compiler deletes is not a test of anything**, and reading one as a
passing grade would have retired a check that was never exercised.

The remaining gap at this boundary is `CFDVizByteSource.cpp`, which has no
direct test of its own; [`ARCHITECTURE.md`](../ARCHITECTURE.md) tracks it.
