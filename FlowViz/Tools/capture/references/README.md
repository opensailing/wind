# Reference stills for blind comparison

This directory holds the reference images the blind harness compares FlowViz
renders against. **It is intentionally empty in source control.**

## Why nothing is committed here

The references that matter are film frames and AAA game captures — *Avatar: The
Way of Water* water and smoke, Houdini/Mantra and Arnold fluid renders,
real-time volumetrics from shipped titles. Those are other people's
copyrighted work. Using them locally to judge our own output is ordinary
comparative evaluation; redistributing them in a public repository is not
something this project gets to decide unilaterally. So they stay out of git,
and `.gitignore` keeps them out by accident as well as on purpose.

The consequence is worth stating plainly: **a clean checkout cannot run a real
blind comparison until someone puts images here.** That is a limitation of the
harness, not a detail to paper over. A review run against an empty corpus has
not compared anything.

## What to put here

```
references/
  presentation/     film and AAA stills — smoke, water, fire, volumetrics
  scientific/       ParaView/Tecplot/EnSight figures, JFM-quality plots
```

Match the profile being reviewed. `Docs/VISUAL_QA.md` is explicit that the two
profiles are held to *different* bars: never fail a Scientific render for
lacking bloom, never pass a Presentation render because it is quantitatively
accurate. A reference from the wrong directory produces a verdict that sounds
authoritative and means nothing.

## Resolution must match

`audit()` reports a dimension mismatch as a leak, and it will refuse to let a
mismatched pair go to a critic. Two reasons, and the second is the one people
forget:

1. `VISUAL_QA.md` §4 — a resolution mismatch invalidates the comparison.
2. It identifies the images. If one is 3840×2160 and the other is 1920×1080,
   the critic knows which is the film still without looking at a pixel.

Crop or capture to a common resolution. Do **not** upscale to match: upscaling
hides aliasing, and aliasing is one of the specific things the review is
looking for.

## Choosing a fair reference

The temptation is to pick a reference the render can beat. That produces a
`PASS` that means nothing, and it is self-deception of exactly the kind
`VISUAL_QA.md` warns about with `WEAK` verdicts.

Pick the reference that a skeptic would pick: same subject, same lighting
situation, same camera distance, and genuinely excellent. If the FlowViz shot
is a backlit smoke plume, the reference is the best backlit smoke plume
available — not a flat-lit one from a different film.
