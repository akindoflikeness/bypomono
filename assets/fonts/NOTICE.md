# Font and asset notice

Every font in this folder has its licence beside it in `<font>/LICENSE.txt`. That
is the gate: **no font enters `assets/fonts/` without its licence file.** It is the
one check that would have caught Xilla.

## The roster

| role | font | licence | attribution |
|---|---|---|---|
| UI chrome, labels, marginalia | Pixeloid Mono | SIL OFL 1.1 | no |
| fallback + the entity's voice | Unifont Ex Mono | SIL OFL 1.1 | no |
| Overworked Acoustic Engineer | Modern DOS | CC0 1.0 | no |
| Archivist Historian | PixAntiqua | SIL OFL 1.1 | no |
| Government Auditor | European Teletext | CC0 1.0 | no |
| Field Psychologist | Pixeloid Sans | SIL OFL 1.1 | no |
| Cosmic Acoustician | Pixel Operator | CC0 1.0 | no |
| Survivor | Better VCR | Open Font License | no |
| Junior Technician | Enter Command | **CC BY 4.0** | **yes** |
| Shell-Listening Monk | Scriptorium | **CC BY 4.0** | **yes** |
| Future Witness | Cyborg Sister | **CC BY 4.0** | **yes** |
| MycelliAI | Alkhemikal | **CC BY 4.0** | **yes** |

## Sprites

Not a font, but it lives under the same gate and Billy asked for it to be
tracked here (2026-08-28).

| role | asset | licence | attribution |
|---|---|---|---|
| the BYPO lever's SPACE prompt | Pixel Keys x16 | custom, see `../sprites/pixel_keys/LICENSE.txt` | no |

Pixel Keys x16 v1.0 by **JoshuaJennerDev** (Heartfire Games), 22 Oct 2022.
Free and commercial use permitted, modification permitted, credit appreciated
but **not required**.

**One term needs watching, because this repository is going public:** the
licence permits use and modification but says *"you cannot repackage,
redistribute, or resell this asset."* Vendoring the whole 111-tile pack into a
public source tree is arguably redistribution, so it is not vendored. What is
here is a single tile — the four-wide spacebar — converted to a 1-bit text grid
by `examples/png_to_grid --alpha`, which is a modification for use in this
project and is the permitted case. **Do not add the rest of the pack.** If more
keys are ever needed, convert those tiles the same way, one at a time.

The grid form is also what the program needs: BYPO decodes no images at
runtime, on purpose — see the note in `fibonacci-gui/Cargo.toml` about keeping
the `image` crate and its dependency tree out.

## Minimum required credit

Four fonts are CC BY 4.0 and **must** be credited. This is the smallest wording
that discharges the obligation — it belongs in the release README and in any
in-app credits:

```
Fonts: Enter Command, Scriptorium, Cyborg Sister and Alkhemikal by jeti
(fontenddev.com), licensed CC BY 4.0. Pixeloid Sans and Pixeloid Mono by
GGBotNet, SIL OFL 1.1. PixAntiqua by Gerhard Grossmann, SIL OFL 1.1.
Unifont Ex Mono by stgiga, built on GNU Unifont, SIL OFL 1.1. Better VCR by
artdzyk, Open Font License. Pixel Operator, Modern DOS and European Teletext by
Jayvee Enaguas, CC0 1.0.
```

That is the lazy version and it is sufficient. Do not ship less than it.

Pixel Keys is not in that block because its licence does not require it. The
line to add if the in-app credits panel wants to be generous anyway:

```
Spacebar key art from Pixel Keys x16 by JoshuaJennerDev.
```

**Still to do:** the credits panel itself. Billy, 2026-08-28: *"we can work in
the credits ui element at the end and ship with credits in the file
structure."* The wording above is what it has to carry; nothing renders it yet.

## The over-the-top version

Billy asked for both, so: the credits should be **set in the fonts they credit.**

Each line is typeset in the face it names. The Junior Technician's credit appears
in Enter Command, the Monk's in Scriptorium, the entity's in Unifont — so the
credits page is simultaneously a specimen sheet and a cast list. It reads as the
instrument introducing its own voices, and it is impossible to fake, because a
missing font shows immediately as a fallback.

Fuller sketch, in the register of the rest of the instrument:

- Present it as a **relic entry of its own** — a thirteenth character whose log
  happens to be the credits, appearing in the voice box like any other. `id` would
  be the next Fibonacci number. The entity's intrusion lines could be the CC0
  fonts, which owe nothing and therefore say nothing.
- Or a dedicated **CREDITS panel** in the WoH idiom: an inverted strip header, one
  row per font, each row showing the font's own name in its own face at its own
  native pixel size (see the grids below — 9 px for the Pixeloids, 16 px for nearly
  everything else), with the licence in the UI face beside it. The size differences
  become the design rather than a problem to normalise.
- The four attribution-required fonts get a marker — a small filled square, say —
  so the *required* credit is visually distinct from the courtesy ones. Honest, and
  it makes the obligation legible rather than buried.

Whichever, the rule holds: a font's own name is always shown in that font.

## Native grids (measured 2026-08-05)

These are bitmap faces, so each is only crisp at whole multiples of the grid it was
drawn on. The number is not a claim off the font's name — every coordinate in a
bitmap face is a multiple of one step, so the GCD of its outlines *is* the step and
`units_per_em / step` is the native height. `font_probe` reports it.

| grid | faces |
|---|---|
| **16 px** | Modern DOS, European Teletext, Pixel Operator, Better VCR, Enter Command, Scriptorium, Cyborg Sister, Alkhemikal, Unifont Ex Mono |
| **9 px** | Pixeloid Mono, Pixeloid Sans |
| none | PixAntiqua — no common step; size it freely |

Two things this corrected. **Modern DOS is 16 px, not 8** — the 8 in `ModernDOS8x16`
is its width, and this document said 8 until the outlines were measured. And
**Unifont Ex Mono is on the 16 px grid** while carrying 62 of the 68 sampled glyphs
as real curves: it is a smoothed build of a bitmap face, which is exactly why it can
be the fallback under a 16 px face without looking like a different size.

The consequence for the interface: 9 and 16 have no common multiple short of 144, so
the UI face and the voice faces cannot both land whole at one size. Pixeloid Mono in
the UI role means UI sizes of 9 and 18. That is a live design question, not a
settled one.

## Provenance notes

- **jeti's four** shipped no licence file and no embedded metadata. dafont's
  "Public domain / GPL / OFL" category is a self-declaration and is not evidence.
  The terms come from the author's own site, quoted and dated in each
  `LICENSE.txt`. If fontenddev.com ever goes down, those quotes are the record.
- **Unifont Ex Mono, PixAntiqua and Better VCR** declared their licence in the
  font's `name` table but shipped no file; their `LICENSE.txt` reproduces the
  declaration plus the OFL 1.1 text.
- **Better VCR** declares "Open Font License" with no version. OFL 1.1 is the only
  published version, so that is what is reproduced.
- **Rejected:** `Minitel` — no licence in the archive, none embedded, and no author
  statement found anywhere. `vhs-vcr-osd` — CC BY-SA 3.0, dropped because
  Share-Alike inside a released binary is a question for a lawyer, not for us.

## Size

`unifontexmono` is 13.7 MB and `better_vcr` 2.8 MB — together ~16.5 MB, most of the
repo. Unifont earns it: it is the only face with complete coverage of φ ρ π Δ • ◦ ¤
and the subscripts, so it is what stops every other font falling back mid-sentence.
If that is too much to carry in git, the alternative is subsetting it to the glyphs
actually used, which is a build step nobody has written yet.
