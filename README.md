# Synthimi
Synthesizers and drum samplers for [ossia score](https://ossia.io).

## Kabang and Minibang pitch envelope

New instances use a percussive pitch ADSR: **0.1 ms attack, 50 ms decay,
zero sustain, 50 ms release**. Enabling the envelope produces a short pitch
sweep back to the Pitch setting rather than holding two octaves above it.
Existing documents and presets retain their saved envelope settings.

Each channel has a **Pitch Env Amount (st)** knob, ranging from **−24 to +24
semitones**, defaulting to **+24** to preserve the previous depth. With a fast
attack and zero sustain, positive amounts fall back to the base pitch and
negative amounts rise back to it. Zero disables the pitch modulation without
changing the ADSR settings. The envelope enable switch still applies.

Velocity tracking scales this signed amount independently; amount and envelope
settings are captured when a note starts. The resulting pitch ratio remains
limited to 0.2–5. The amount shows with each channel's pitch controls, but its
ports come after all the channels' so that the other ports keep their ids:
cables and presets saved before it still reach the controls they were made for.

Pitch release starts on MIDI note-off (including a zero-velocity note-on)
and when the sample input ends. It applies in both Kabang and Minibang.
Amplitude and filter envelope defaults are unchanged.

The sampler uses RubberBand's `OptionPitchHighConsistency` for changing pitch
ratios. This remains windowed, duration-preserving pitch shifting, not
sample-accurate variable-rate playback.

Minibang retains both channel and global Volume controls for compatibility.
They multiply together; unlike Kabang's eight-channel mix, Minibang does not
need separate channel balancing and master gain.

## Kaboom electronic kits

The default package's `Presets/Kaboom` collection includes these twenty
electronic kits, categorized as **Electronic**. They are original sound
designs inspired by drum-machine roles, not hardware emulations.

| Kit | Character | Suggested BPM |
| --- | --- | ---: |
| Circuit Eight | Rounded sub electro, rubber toms | 116 |
| Circuit Nine | Punchy house and techno | 128 |
| Pocket Six | Tight, light acid rhythms | 132 |
| Neon Twelve | Dry digital electro | 112 |
| Chicago Jack | Warm jack-house, broad clap | 124 |
| Detroit Current | Tuned minimal techno | 126 |
| Acid Switch | Driven acid techno | 138 |
| Garage Shuffle | Short voices for swung house | 130 |
| Disco Relay | Bright disco-house, clap emphasis | 122 |
| Dub Circuit | Dark dub-techno and low toms | 120 |
| Warehouse Pulse | Driving modern techno | 132 |
| Hardline Impact | Saturated industrial percussion | 142 |
| Minimal Voltage | Compact, dry minimal house | 124 |
| Liquid Breaks | Smooth drum'n'bass | 174 |
| Neuro Circuit | Aggressive drum'n'bass, FM attacks | 172 |
| Jungle Terminal | Layered breakbeat snare | 168 |
| Broken Grid | Asymmetric IDM percussion | 112 |
| Sub Station | Heavy halftime and dubstep | 140 |
| Trap Capacitor | Long sub kick, crisp hats and clap | 144 |
| Glitch Register | Short digital ticks, zaps and stutters | 128 |

Every kit uses the same MIDI note numbers:
**36 kick, 38 snare, 39 clap, 42 closed hat, 46 open hat,
45 low tom, 50 high tom, 75 auxiliary percussion**.
The hats share choke group 1; other voices are unchoked.
BPM suggestions are not stored tempos or sequenced patterns.

Kicks use phase-reset sine FM with zero FM index and zero tension/cascade,
avoiding the history-dependent tuning of a still-ringing nonlinear membrane.
Noise-forward snares and claps, short noise/granular hats, and compact FM
accents replace the long metallic resonances of the physical kits.
The existing physical presets are unchanged.

## Kaboom experimental kits

Eight additional kits appear under **Experimental** in the default package.
These use the same MIDI mapping as the electronic collection, with notes
45, 50 and 75 serving as low gestures, high gestures and auxiliary textures
rather than necessarily as conventional toms.

| Kit | Character | Suggested BPM |
| --- | --- | ---: |
| Torsion Logic | Dry rubber FM, uneven ratchets, tense synthetic joints | 137 |
| Oxide Memory | Corroded industrial body, blurred noise, short metallic haze | 102 |
| Elastic Topology | Inflated impacts, glossy pitch bends, synthetic tearing | 145 |
| Microfracture | Pointillist clicks, tiny fragments, clean low anchor | 126 |
| Pressure Mesh | Sub pressure, rough noise bursts, low resonant friction | 118 |
| Counterphase | Detailed broken-techno transients and rim clusters | 138 |
| Lichen Circuit | Organic-digital grains, soft asymmetry, woody mutations | 109 |
| Fracture Bloom | Dense industrial bursts, swelling noise, sharp FM bends | 154 |

Positive and negative pitch envelopes, non-integer FM ratios, accelerating
and decelerating strike clusters, and sparse particle excitation make these
kits more gestural than the conventional drum-machine collection. Kicks
remain phase-reset FM with no energy-dependent tension; brief FM transients
are intentional. Lichen Circuit also uses small amounts of flam-timing
randomness on its clustered texture voices.

These are sound palettes, not sequencers or reproductions of particular
artists. The BPM values are suggestions; the presets do not install rhythms.
