# Writing the relic log

`relic_log.json` is the instrument's only voiced text. Characters own their
entries, so **adding a line to an existing witness is four lines, and adding a new
witness is one block.** Hot-reloaded every ~2 s — save and it's live.

## Adding an entry to someone who already exists

Find their character, append to `entries`:

```json
{
  "tstamp": "1987-04-15::02:40",
  "alias": "op5_thermal",
  "extra": "fb:0.912 held",
  "log": [
    "The bench is warm and nothing is plugged in.",
    "I checked twice. Nothing is plugged in."
  ]
}
```

Only the lines are required. `tstamp`, `alias` and `extra` are the found-document
furniture and can be left out.

## Adding a new character

Append a block to `characters`:

```json
{
  "archetype": "Night Porter",
  "era": "1971",
  "rarity": "uncommon",
  "portrait": "porter_1bit",
  "entries": [
    { "log": ["It hums when the building is empty. Only then."] }
  ]
}
```

Then a portrait at `assets/portraits/porter_1bit.txt` — see
[`portraits/README.md`](portraits/README.md). **Until that file exists the plinth
shows a turning shell mark**, so a new character works immediately and the art can
follow whenever.

## The fields

| field | where | what it does |
|---|---|---|
| `archetype` | character | who they are. Shown on the box once the metadata strip lands. |
| `era` | character | flavour. Any string — `1987`, `present`, `ageless`. |
| `rarity` | character | **selection weight**: `common` 8, `uncommon` 5, `rare` 3, `very_rare` 1. Fibonacci, so a common entry comes up eight times as often as a very rare one. Anything unrecognised counts as very rare. |
| `portrait` | character | basename of the portrait file. Extension optional — `porter_1bit`, `porter_1bit.png` and `porter_1bit.txt` all resolve to `portraits/porter_1bit.txt`. |
| `log` | entry | what a person said. Lines are shown as one block and typed out. |
| `intrusion` | entry | what the **entity** says. Rendered in inverted type — white box, black text. |
| `tstamp` `alias` `extra` | entry | metadata. Parsed now, displayed once the strip lands. |

Any of `archetype`, `era`, `rarity`, `portrait` can also be set **on an entry** to
override the character for that one line — which is how the eight Logalith
intrusions keep their individual A–H labels while sharing one portrait and one
rarity.

## Two things worth knowing

**Ids are assigned, not written.** Each entry gets the next distinct Fibonacci
number by position — 1, 2, 3, 5, 8, 13 … — so there's nothing to keep track of.
Inserting a character in the middle renumbers everything after it, which is
harmless; ids are display flavour, nothing keys off them.

**Who speaks when.** An entry with `log` is a witness, and witnesses speak while
φ integrity holds. An entry with only `intrusion` is the entity, and it speaks
either because **agitation** has built up — a leaky integral of `max(rip, haunt)`,
so sustained pushing earns it and relenting drains it — or because of a standing
8 % chance representing the interface's own decay. An entry with **both** has its
log cut off mid-sentence by its intrusion once agitation is high enough.

So `intrusion`-only entries are punishment, and `log`+`intrusion` entries are
someone being interrupted. That distinction is the only mechanical one in the file.

## Format notes

A UTF-8 BOM is stripped on load, because PowerShell writes one by default and
`serde_json` treats it as a syntax error at line 1 — which would silence the whole
log. If you edit with `Set-Content`, pass `-Encoding utf8`.

The original flat-array format still parses, so an old copy pasted back in won't
break anything.
