# Font notice

Every font in this folder has its licence beside it in `<font>/LICENSE.txt`.
No font enters `assets/fonts/` without its licence file.

| role | font | author | licence | attribution required |
|---|---|---|---|---|
| UI text | Pixeloid Mono | GGBotNet | SIL OFL 1.1 | no |
| readouts | European Teletext | Jayvee Enaguas | CC0 1.0 | no |
| fallback for glyphs the others lack (φ ρ π Δ • ◦ ¤, subscripts) | Unifont Ex Mono | stgiga, built on GNU Unifont | SIL OFL 1.1 | no |

## Native grids

These are bitmap faces and are only crisp at whole multiples of the grid
they were drawn on: Pixeloid Mono 9 px, European Teletext 16 px, Unifont Ex
Mono 16 px. `ui_font()` and `readout_font()` in `src/gui/text.c` snap to
these.

## Provenance

- Unifont Ex Mono declared its licence in the font's `name` table but shipped
  no file; its `LICENSE.txt` reproduces the declaration plus the OFL 1.1 text.
- Unifont Ex Mono is 13.7 MB, most of the repository. It is the only face with
  complete coverage of the symbols above, which is what stops the other two
  falling back mid-sentence.
