# Font notice

Every font's licence text is recorded in `THIRD-PARTY-LICENSES.txt` at the
repo root. No font enters `assets/fonts/` without its licence recorded there.

| role | font | author | licence | attribution required |
|---|---|---|---|---|
| all text | BYPOSerif, a bitmap cut of Source Serif 4 | Adobe (outlines) | SIL OFL 1.1 | no |
| fallback for glyphs BYPOSerif lacks (φ ρ π Δ • ◦ ¤, subscripts) | Unifont Ex Mono | stgiga, built on GNU Unifont | SIL OFL 1.1 | no |

## Native grids

BYPOSerif is drawn pixel by pixel at 10, 11, 12, 13, 16 and 18 px and has
no outlines, so it exists at those sizes only; `ui_font()` in
`src/gui/text.c` picks from them. Unifont Ex Mono is a bitmap face drawn on a
16 px grid and is only crisp at multiples of it.

## Provenance

- Unifont Ex Mono declared its licence in the font's `name` table but shipped
  no file; `THIRD-PARTY-LICENSES.txt` reproduces the declaration plus the OFL
  1.1 text.
- Unifont Ex Mono is 13.7 MB, most of the repository. It is the only face with
  complete coverage of the symbols above, which is what stops BYPOSerif
  falling back mid-sentence.
