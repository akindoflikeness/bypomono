# Portrait image brief

Copy-pasteable spec for generating or selecting the eleven portrait sources. Written
to be handed to an image model or an assistant verbatim.

---

## THE BRIEF

I need a source image that will be **dithered to pure 1-bit black-and-white and
displayed at 80 × 56 pixels**. Everything below follows from that.

**Format**

- Aspect ratio **10:7 landscape (1.43:1)**. Not portrait, not square.
- Generate large — 1120 × 784 or above — then it gets downscaled. Detail below
  ~1/14 of the frame will not survive, so do not rely on any.
- Greyscale or easily desaturated. Colour is discarded entirely.

**Tone and contrast**

- **Three tonal zones, no more**: a clear light, a clear dark, one mid. The image is
  reduced to two colours plus dither, so anything relying on subtle gradation
  disappears.
- **One strong light source**, hard-edged, ideally from the side. Deep shadow is an
  asset, not a problem.
- The subject must be separable from its background by **value alone** — squint at
  it in greyscale; if the subject and background are the same brightness, it will
  not read.
- **Avoid**: flat even lighting, low contrast, busy or textured backgrounds, fine
  patterns (fabric weave, hair strands, foliage detail), thin lines, small text.

**Composition**

- **Head and shoulders**, subject occupying 60–80% of the frame height.
- **Three-quarter view or downcast** reads better than straight-to-camera at this
  size: less flat skin to go blotchy in the dither.
- Plain or near-plain background, ideally one that can be masked out.
- Silhouette matters more than features. If the outline is unreadable, no amount of
  interior detail rescues it.

**What it is for**

A 1-bit portrait on a striped field in a monochrome instrument panel — the register
of 1970s–80s lab equipment, found documents, degraded records. Not glossy, not
cinematic. It should look like it was *recovered*, not shot.

---

## The eleven subjects

Swap the subject line into the brief above. Two of these are not people.

| file | subject line |
|---|---|
| `engineer_1bit` | An exhausted acoustic engineer at a workbench, 1987, shirt sleeves, hand at his forehead, past caring. |
| `juniortech_1bit` | A young technician, present day, slightly sheepish, caught having made a mistake. |
| `psych_1bit` | A field psychologist, 2014, clinical and composed, taking notes on someone else's dream. Neutral expression doing a lot of work. |
| `archivist_1bit` | An institutional archivist, 1959, spectacles and cardigan, uneasy about the item they are cataloguing. |
| `acoustician_1bit` | An acoustician, near future, listening to something enormous on purpose. Awed rather than frightened. |
| `survivor_1bit` | A survivor in ruins, era unknown, dirt and improvised gear, the eyes doing the talking. |
| `monk_1bit` | A hooded monk, downcast, devotional, listening to a shell on an altar. Could be any century. |
| `future_1bit` | A witness living after a disaster where a day became 28 hours. Worn, adapted, unbothered. The aftermath, not the future. |
| `auditor_1bit` | A government auditor, 1993, badge and lanyard, bureaucratic, faintly absurd. |
| `logalith_1bit` | **Not a person.** An ancient geometric object that does not want to be looked at — a shell interior, a carved boss, something that resembles an eye without being one. |
| `mycelliai_1bit` | **Not a person.** A fungal network: hyphae, branching filaments, a forest canopy seen from below. |

Per-subject notes:

- **`survivor_1bit`** is the one to leave rough. Everywhere else, clean the dither by
  hand; there the noise is characterisation.
- **`logalith_1bit`** appears beside inverted text — a white box, not a black one —
  since that is when the entity speaks.
- **`logalith_1bit`** and **`mycelliai_1bit`** should not be forced into portrait
  framing.

---

## Getting it in

1. Dither to 1-bit — [Dithermark](https://app.dithermark.com/) has the widest choice
   of algorithms. Export a **spread of settings**, not one.
2. Resize to **80 × 56**, nearest neighbour.
3. Mask the background to transparent if you want the panel's stripes showing
   through behind the figure.
4. Convert the whole folder and keep only the usable densities:

```bash
cargo run -p fibonacci-gui --example png_to_grid -- --dir <folder> --only-good
```

Every surviving export becomes an animation frame — they get ordered by ink density
and cycled, so the grain churns. A dozen variants is a feature, not waste.

**Target 25–60% ink, ideally 30–45% for a face.** Below that it reads as scattered
noise; above it the figure becomes a slab and the stripes stop showing through. The
converter flags anything outside the band and tells you which way it missed.

**If a grid comes out empty or solid**, run `--stats`. Mostly-transparent plus
near-black means the shape is in the alpha channel — add `--alpha`. Nothing above
the threshold means faint anti-aliased lines — lower `--threshold`.

---

## Why 80 × 56

The panel Z occupies is **574 × 412 px, aspect 1.39**, and the app reports its own
figure to the log on startup and on any resize — trust that over this document.

Scaling is whole-number only, so a 80 × 56 grid draws at ×7 = **560 × 392: 98% of
the width, 95% of the height.** A 64 × 80 portrait grid, by contrast, can only fill
the height — ×5 = 320 × 400, which is **56% of the width** and leaves nearly half
the panel empty. The aspect is what matters, not the pixel count: 80 × 56 is *fewer*
pixels than 64 × 80 and fills far more of the panel, because matching the shape lets
the scale factor go up.
