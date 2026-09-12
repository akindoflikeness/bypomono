# The eleven portraits

Drop each file in this folder. **64×80 px** is the working size and **128×120 the
cap**, dithered to two colours, background **transparent** (not filled black — empty
cells are where the plinth's scanlines show through). Hot-reloaded every ~2 s, so
save and watch it land.

**In the text grid, only a space is empty.** Every other character is ink, including
`.` and `0`. Those two used to count as background, which silently hollowed out any
art that used them as a tone — see the status note below.

Format and workflow: [`tools/aseprite/README.md`](../../../../tools/aseprite/README.md).

Ordered by how often each one speaks, so the ones worth doing first are at the
top. `engineer` and `juniortech` are `common` and will come up most; `logalith`
covers eight entries on its own.

| # | filename | who | era | speaks | what to look for in a reference |
|---|----------|-----|-----|--------|--------------------------------|
| 1 | `logalith_1bit.txt` | Logalith Intrusions A–H | ageless | 8 entries | **Not a person.** The entity itself. Its lines are the ALL-CAPS ones with the `•◦¤φ` sigils, and it speaks in inverted type. Something geometric, ancient and unwelcoming — a shell interior, a carved boss, an eye that isn't one. |
| 2 | `engineer_1bit.txt` | Overworked Acoustic Engineer | 1987 | 3 (common) | Exhausted man at a bench. *"If the carriers keep folding into φ I'm filing the ticket."* Shirt sleeves, bad lighting, a look of being past caring. |
| 3 | `juniortech_1bit.txt` | Junior Technician | present | 3 (common) | Young, present-day, slightly sheepish. *"Patched op3 → op4. Forgot to save."* The one who typed 'nasty' into the preset box. |
| 4 | `psych_1bit.txt` | Field Psychologist | 2014 | 3 (uncommon) | Clinical, composed, taking notes on someone else's dreams. *"They drew a spiral and then apologised."* Neutral expression doing a lot of work. |
| 5 | `archivist_1bit.txt` | Archivist Historian | 1959 | 3 (uncommon) | 1959 institutional. *"The ledger smells of solder and lemon."* Spectacles, cardigan, a person who files things and is uneasy about this one. |
| 6 | `acoustician_1bit.txt` | Cosmic Acoustician | 2029 | 3 (uncommon) | Near-future, awed rather than frightened. *"Measured a return that matched my heartbeat."* Someone listening to something enormous on purpose. |
| 7 | `survivor_1bit.txt` | Survivor | unknown | 3 (rare) | Found it in ruins. *"Don't sing the mirror."* Dirt, improvised gear, eyes doing the talking. The most degraded dither of the set — noise is right here. |
| 8 | `monk_1bit.txt` | Shell-Listening Monk | unknown | 1 (rare) | *"I fear we are not praying. I fear we are being tuned."* Hood, downcast, devotional. Could be any century. |
| 9 | `future_1bit.txt` | Future Witness | post-disturbance | 1 (rare) | Lives where a day is 28 hours. *"Nobody remembers what we lost to make room for it."* Should look like the aftermath rather than the future — worn, adapted, unbothered. |
| 10 | `auditor_1bit.txt` | Government Auditor | 1993 | 1 (uncommon) | *"I do not wish to audit motivation."* Bureaucratic, 1993, badge-and-lanyard energy. Faintly absurd, which is the joke. |
| 11 | `mycelliai_1bit.txt` | MycelliAI Forest Agent | post-disturbance | 1 (very rare) | **Not a person either.** A global fungal network reporting a war crime against irrationality. *"Life is not meant to be rational. Irrationality is our bloom."* Hyphae, branching, a canopy from below. The only entry that gets interrupted mid-sentence by the Logalith. |

## Status (2026-08-05)

Billy delivered ten portraits as one bundle, `zboxtxtportraits.txt`, sections marked
`/name`. They have been split into the grid files beside it. The bundle is working
material — the instrument never loads it, and it can be deleted once the split files
are confirmed.

**Filled — 5 of 11:** `logalith_1bit`, `monk_1bit`, `survivor_1bit` (from the new
batch), `engineer_1bit` (13 frames) and `acoustician_1bit` (from the first).

**Delivered but unassigned — 4.** These are Billy's own labels; only he knows which
archetype each is meant to be. Rename to the slot's filename and they are live:

| file | size | ink |
|---|---|---|
| `human1.txt` | 98×53 | 3,494 |
| `human2.txt` | 100×55 | 2,871 |
| `human3.txt` | 102×56 | 3,402 |
| `non_human1.txt` | 99×52 | 749 |

The six empty slots are `juniortech`, `psych`, `archivist`, `future`, `auditor` and
`mycelliai` — two of which (`mycelliai` especially) are the not-a-person ones, so
`non_human1` is the obvious candidate for it.

**Delivered but not in the log — 3.** `cultist.txt` (100×97), `schism_leader.txt`
(100×72) and `borg_cube_easter_egg.txt` (96×50) are characters `relic_log.json` has
never heard of. They need entries written before they can ever appear, and those
lines are Billy's to write — see [`../RELIC_LOG.md`](../RELIC_LOG.md); a new witness
is one block.

**What the batch broke on arrival**, both fixed and both now covered by a test that
parses every grid in this folder: nine of the ten were 98–120 wide against a cap of
96 and were refused outright, and four used `0` or `.` as a tone, which the loader
read as background — `non_human1` arrived as 28 marks out of 749.

## Notes

- **Two of the eleven aren't faces** — `logalith` and `mycelliai`. Don't force a
  portrait on either.
- **`survivor` wants a rough dither.** For everyone else, clean it up by hand;
  here the noise is characterisation.
- The `logalith` portrait shows during **inverted** text, so bear in mind it sits
  next to a white box rather than a black one.
- Faces don't need to look at the camera. Image 5's do, and it works, but a
  three-quarter or downcast crop dithers better at this size because there's less
  flat skin to go blotchy.
- If a file is missing, that plinth shows a turning shell mark instead — nothing
  breaks, so do them in any order.
