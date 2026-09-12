# BYPO_mono

# One Tone, Many Voices

Blow Your Phase Off is an untamed opinionated instrument designed using mathematical irrationality to access uniquely stable inharmonic timbres.
The architecture is PM using a dual algorithm selector. The arrangement of modulators, carriers and feedback are selected using
roman numerals 1...8 and the ratio mode is selected using the ratio mode selector providing 5 separate irrational ratio configurations. PM modulation depth is tuned via these recursive algorithms.

# Operator Structures

| # | ID | Structure | Carriers | Max depth | Feedback on |
|---|----|-----------|----------|-----------|-------------|
| I    | `SSSS` | full FM tree: `(1→2→3)` and `4` both modulate `5` | 1 | 4 | op 1 (depth 4) |
| II   | `SSSP` | true 4-op stack `1→2→3→4`, plus sine `5` | 2 | 4 | op 1 (depth 4) |
| III  | `PSSP` | 3-op stack `1→2→3`, plus sines `4`, `5` | 3 | 3 | op 1 (depth 3) |
| IV   | `PPSP` | FM pair `1→2`, plus sines `3`, `4`, `5` | 4 | 2 | op 1 (depth 2) |
| V    | `PPPP` | five parallel sines (pure additive) | 5 | 1 | op 1 (depth 1) |
| VI   | `PPPF` | FM pair `4→5`, plus sines `1`, `2`, `3` | 4 | 2 | **op 5 (depth 1)** |
| VII  | `PPFF` | FM pairs `1→2` and `4→5`, plus sine `3` | 3 | 2 | **op 2 (depth 1)** |
| VIII | `PFFF` | 3-op stack `1→2→3` and FM pair `4→5` | 2 | 3 | **op 3 (depth 1)** |

# Phase Violence

There are some additional controls that allow for even deeper timbric exploration that I encourage you to experiment with those being
RIP, haunt, and ghost. Operators can be turned off removing not only its signal from the output but from the modulation signal chain.
I'd highly recommend experimenting with turning off operators to find unique mixtures (especially in conjunction with The Room's ghost).

# Chambers

The Room is a uniquely close sounding reverb with a dirty tank and a modulation linked dampener (via field).
It has its own tricks with haunt and ghost that have novel effects, and of course The Room was structured using fibonacci recursion.
Here it was used to define phase offsets via the all pass filter networks.

# Progenitor

Chandas listens to the signal before and after we do. It's a unique granular delay that can sync with the internal sequencer.
Whilst syncing it has a polyrhythmic texture though Chandas also features its own massive lush reverb with two multi controls to tune
its bloom.

# Textural Utility

And finally warmth, it's mainly an output level utility - with some extra juice packed into the slider's range. Near the top end
of its range it starts upward multiband compression, even-bias clipping, spectral processing and an auto-release limiter stage.

# Output

You can record takes within the application that records 48khz 32bit float .wav files to your Music folder.
The instrument runs at 48khz sample rate, selects your default output device and is fully midi compatible.
A console accessed using "/" is available for mapping MIDI CC to parameters.
A release is planned for Linux, Windows and MacOS as a standalone application and CLAP plugin for DAWs.
