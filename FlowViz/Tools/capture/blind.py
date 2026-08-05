"""Blind side-by-side comparison harness for the visual review loop.

`Docs/VISUAL_QA.md` section 3 requires that the critic be shown the FlowViz
render and a film/AAA/publication reference **without being told which is
which**. That requirement is easy to state and easy to violate by accident,
which is why it is code rather than a note in a prompt.

WHAT ACTUALLY BREAKS A BLIND TEST

Not the shuffle -- the shuffle is the easy part. What breaks it is metadata:

  * The candidate is always presented first, so the critic learns after one
    round that A is always ours.
  * A filename survives ("flowviz_frame_0012.png" next to "avatar_still.png").
  * The two images are different sizes, so the 3840-wide one is obviously the
    film still. Section 4 already forbids mismatched resolution because it
    invalidates the comparison; it is also a perfect identity leak.
  * A renderer stamped itself into a PNG tEXt chunk ("Software: Unreal
    Engine").
  * The answer key was written next to the images the critic was pointed at.

`seal()` prevents the first two by construction and `audit()` reports the
rest. Both must run before a critic is asked anything: a leaked comparison
does not produce a weaker verdict, it produces a worthless one, and it is
indistinguishable from a real verdict afterwards.

WHY THE PARSER IS STRICT

`should_continue()` treats WEAK as continue, per the protocol. And a PASS
whose WHAT WORKS section says only "looks good" is refused outright, because
section 3 requires that a pass state specifically what makes it good. Refusing
it at parse time is the difference between a rule and a suggestion.
"""

import json
import os
import random
import re
import struct


LEAK_DIMENSIONS = "dimensions"
LEAK_NAME = "name"
LEAK_TEXT_CHUNK = "text-chunk"
LEAK_RECORD_VISIBLE = "record-visible"

VERDICTS = ("REJECT", "WEAK", "PASS", "EXCEEDS")
PASSING = ("PASS", "EXCEEDS")

# Substrings in a presented filename that would tell the critic which is which.
_TELLS = (
    "flowviz",
    "candidate",
    "cand",
    "reference",
    "ref",
    "render",
    "ours",
    "mine",
    "theirs",
    "film",
    "avatar",
    "houdini",
    "arnold",
    "paraview",
    "unreal",
    "ue5",
)

# A WHAT WORKS entry has to carry information. These are the shrugs.
_VAGUE = (
    "looks good",
    "looks great",
    "looks nice",
    "looks fine",
    "good",
    "great",
    "nice",
    "fine",
    "ok",
    "okay",
    "pretty",
    "beautiful",
    "impressive",
    "works",
    "works well",
    "all good",
    "no issues",
    "nothing wrong",
    "seems fine",
)


class Sealed(object):
    """One blinded pairing: what the critic sees, and where the key lives."""

    __slots__ = ("a_path", "b_path", "record_path", "assignment")

    def __init__(self, a_path, b_path, record_path, assignment):
        self.a_path = a_path
        self.b_path = b_path
        self.record_path = record_path
        self.assignment = assignment

    def __repr__(self):
        return "Sealed(a=%r, b=%r)" % (
            os.path.basename(self.a_path),
            os.path.basename(self.b_path),
        )


class Leak(object):
    """Something that lets the critic recover the identities."""

    __slots__ = ("kind", "detail")

    def __init__(self, kind, detail):
        self.kind = kind
        self.detail = detail

    def __eq__(self, other):
        if not isinstance(other, Leak):
            return NotImplemented
        return self.kind == other.kind and self.detail == other.detail

    def __repr__(self):
        return "Leak(%s: %s)" % (self.kind, self.detail)


class Report(object):
    """A parsed critic report."""

    __slots__ = ("profile", "verdict", "blind", "hard_failures", "quality_gaps",
                 "what_works")

    def __init__(self, profile, verdict, blind, hard_failures, quality_gaps,
                 what_works):
        self.profile = profile
        self.verdict = verdict
        self.blind = blind
        self.hard_failures = hard_failures
        self.quality_gaps = quality_gaps
        self.what_works = what_works

    def __repr__(self):
        return "Report(%s, %s, %d hard failures)" % (
            self.profile,
            self.verdict,
            len(self.hard_failures),
        )


def _copy(src, dst):
    with open(src, "rb") as source:
        data = source.read()
    with open(dst, "wb") as target:
        target.write(data)


def seal(candidate_path, reference_path, shown_dir, record_path, seed=None):
    """Copy the two images into `shown_dir` as A.png and B.png in random order.

    The critic is pointed at `shown_dir` and nothing else. The mapping goes to
    `record_path`, which MUST be outside `shown_dir` -- an answer key sitting
    beside the images is not a blind test, and the critic will list that
    directory.

    `seed` makes a session reproducible. Leave it None for a real review.
    """
    shown_dir = os.path.abspath(shown_dir)
    record_path = os.path.abspath(record_path)

    if os.path.dirname(record_path) == shown_dir or record_path.startswith(
        shown_dir + os.sep
    ):
        raise ValueError(
            "the answer key would be written inside the directory shown to the "
            "critic (%s), which makes the comparison open-book" % shown_dir
        )

    if not os.path.isdir(shown_dir):
        os.makedirs(shown_dir)

    rng = random.Random(seed)
    candidate_is_a = rng.random() < 0.5

    a_source = candidate_path if candidate_is_a else reference_path
    b_source = reference_path if candidate_is_a else candidate_path
    assignment = {
        "A": "candidate" if candidate_is_a else "reference",
        "B": "reference" if candidate_is_a else "candidate",
    }

    a_path = os.path.join(shown_dir, "A.png")
    b_path = os.path.join(shown_dir, "B.png")
    _copy(a_source, a_path)
    _copy(b_source, b_path)

    with open(record_path, "w") as handle:
        json.dump(
            {
                "assignment": assignment,
                "candidate_source": os.path.abspath(candidate_path),
                "reference_source": os.path.abspath(reference_path),
                "shown_dir": shown_dir,
            },
            handle,
            indent=2,
        )

    return Sealed(a_path, b_path, record_path, assignment)


def _png_dimensions(path):
    """Width and height from the IHDR, or None if this is not a PNG."""
    with open(path, "rb") as handle:
        head = handle.read(24)
    if len(head) < 24 or head[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    return struct.unpack(">II", head[16:24])


def _png_text_chunks(path):
    """Every tEXt/iTXt/zTXt payload, as bytes. Renderers stamp their name here."""
    found = []
    with open(path, "rb") as handle:
        if handle.read(8) != b"\x89PNG\r\n\x1a\n":
            return found
        while True:
            header = handle.read(8)
            if len(header) < 8:
                break
            length, tag = struct.unpack(">I", header[:4])[0], header[4:8]
            if tag == b"IEND":
                break
            # Cap the read: a corrupt length field must not allocate wildly.
            payload = handle.read(min(length, 1 << 20))
            handle.read(4)  # CRC
            if tag in (b"tEXt", b"iTXt", b"zTXt"):
                found.append(payload)
    return found


def audit(sealed):
    """Return every way the critic could recover the identities.

    An empty list means the pairing is fit to review. Run this BEFORE asking
    the critic anything: a leaked comparison is not a weaker signal, it is a
    worthless one, and afterwards it is indistinguishable from a real verdict.
    """
    leaks = []

    a_name = os.path.basename(sealed.a_path)
    b_name = os.path.basename(sealed.b_path)
    for name in (a_name, b_name):
        stem = os.path.splitext(name)[0].lower()
        if stem in ("a", "b"):
            continue
        hit = [tell for tell in _TELLS if tell in stem]
        if hit:
            leaks.append(
                Leak(
                    LEAK_NAME,
                    "presented file %r contains identifying text %r" % (name, hit[0]),
                )
            )
        else:
            leaks.append(
                Leak(
                    LEAK_NAME,
                    "presented file %r is not named A or B, so the pairing is "
                    "not anonymous" % name,
                )
            )

    a_dims = _png_dimensions(sealed.a_path)
    b_dims = _png_dimensions(sealed.b_path)
    if a_dims and b_dims and a_dims != b_dims:
        leaks.append(
            Leak(
                LEAK_DIMENSIONS,
                "the images are %dx%d and %dx%d; a resolution mismatch both "
                "invalidates the comparison (VISUAL_QA section 4) and reveals "
                "which is the film reference"
                % (a_dims[0], a_dims[1], b_dims[0], b_dims[1]),
            )
        )

    for path in (sealed.a_path, sealed.b_path):
        for payload in _png_text_chunks(path):
            text = payload.decode("utf-8", "replace").lower()
            hit = [tell for tell in _TELLS if tell in text]
            if hit:
                leaks.append(
                    Leak(
                        LEAK_TEXT_CHUNK,
                        "%s carries PNG metadata naming its source (%r)"
                        % (os.path.basename(path), hit[0]),
                    )
                )

    shown_dir = os.path.dirname(sealed.a_path)
    for entry in sorted(os.listdir(shown_dir)):
        if entry in (a_name, b_name):
            continue
        leaks.append(
            Leak(
                LEAK_RECORD_VISIBLE,
                "%r sits in the directory shown to the critic; only the two "
                "images may be there" % entry,
            )
        )

    return leaks


def reveal(record_path, choice):
    """Map the critic's stated preference ("A" or "B") back to an identity."""
    key = str(choice).strip().upper()
    if key not in ("A", "B"):
        raise ValueError(
            "choice must be 'A' or 'B', got %r -- the critic must name one of "
            "the two presented images" % (choice,)
        )
    with open(record_path) as handle:
        record = json.load(handle)
    return record["assignment"][key]


def _section(text, heading):
    """Bullet lines under a heading, up to the next all-caps heading."""
    pattern = re.compile(
        r"^%s\s*$(.*?)(?=^[A-Z][A-Z ]{2,}\s*$|\Z)" % re.escape(heading),
        re.MULTILINE | re.DOTALL,
    )
    match = pattern.search(text)
    if not match:
        return []
    items = []
    for line in match.group(1).splitlines():
        line = line.strip()
        if line.startswith("-"):
            items.append(line.lstrip("- ").strip())
    return [item for item in items if item.lower() not in ("none", "n/a", "")]


def parse_report(text):
    """Parse a critic report in the format required by VISUAL_QA section 3.

    Raises ValueError on anything that is not a usable verdict -- including a
    PASS with no specific praise, which the protocol forbids.
    """
    verdict_match = re.search(r"^VERDICT:\s*(\S+)", text, re.MULTILINE)
    if not verdict_match:
        raise ValueError(
            "the report has no VERDICT line; the required format is set out in "
            "Docs/VISUAL_QA.md section 3"
        )

    verdict = verdict_match.group(1).strip().upper()
    if verdict not in VERDICTS:
        raise ValueError(
            "unknown verdict %r; must be one of %s. 'Looks good' is not a "
            "verdict -- the protocol requires one of these four so the loop "
            "can act on it." % (verdict, ", ".join(VERDICTS))
        )

    profile_match = re.search(r"^PROFILE:\s*(.+)$", text, re.MULTILINE)
    profile = profile_match.group(1).strip() if profile_match else ""

    blind_match = re.search(
        r"^BLIND:\s*(.*?)(?=^[A-Z][A-Z ]{2,}\s*$|\Z)", text, re.MULTILINE | re.DOTALL
    )
    blind = " ".join(blind_match.group(1).split()) if blind_match else ""

    hard_failures = _section(text, "HARD FAILURES")
    quality_gaps = _section(text, "QUALITY GAPS")
    what_works = _section(text, "WHAT WORKS")

    if verdict in PASSING:
        specific = [
            item for item in what_works if item.rstrip(".").lower() not in _VAGUE
        ]
        if not specific:
            raise ValueError(
                "a %s verdict must say specifically what makes the render good "
                "(VISUAL_QA section 3: \"'Looks good' is not a passing "
                "verdict\"). The WHAT WORKS section is empty or vague: %r"
                % (verdict, what_works)
            )
        what_works = specific

        if hard_failures:
            raise ValueError(
                "a %s verdict cannot coexist with hard failures: %r"
                % (verdict, hard_failures)
            )

    return Report(profile, verdict, blind, hard_failures, quality_gaps, what_works)


def should_continue(verdict):
    """True if the /loop must run another iteration.

    WEAK continues. The protocol calls it the most common self-deception in
    this kind of work: "no specific thing is wrong" feels like success while
    the result is still visibly inferior to the reference.
    """
    key = str(verdict).strip().upper()
    if key not in VERDICTS:
        raise ValueError("unknown verdict %r" % (verdict,))
    return key not in PASSING
