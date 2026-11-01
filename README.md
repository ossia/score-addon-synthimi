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

## Cordito string synthesizer

Cordito is a polyphonic physical model of strings: 16 voices, 39 controls, and
four engines selected by **Engine**:

| Engine | Model | Exciters |
| --- | --- | --- |
| Pluck | Digital waveguide: two delay lines meeting at the excitation point, a loss filter and up to four dispersion allpasses at the bridge | Pick, Finger, Hammer, Burst |
| Struck | Up to 48 partials of a stiff string per course (two courses with Detune), struck by a Hunt-Crossley felt hammer | Hammer, Mallet, Pick, Burst |
| Bow | The same waveguide with a friction junction at the bow point; Force is the bow pressure, Bow Speed, Bow Attack and Bow Lift its motion | Bow, E-Bow |
| Buzz | Pluck with a lossless, amplitude-dependent bridge: the string buzzes against a curved bridge | Pick, Finger, Burst |

An exciter the engine does not have falls back to the engine's default, so a
preset never goes silent because of a combination.

A **Pick** or a **Finger** lets go of the string: its partials fall as 1/n, as
a plucked string's do, with the plectrum's or the finger's roll-off above
**Hardness**. A **Hammer** or a **Mallet** strikes it, with a flat spectrum up
to the felt's. Velocity sets the brightness as well as the level of every
exciter (Jaffe and Smith's dynamics filter). A struck string's hammer is in
contact for at most about half a period, so the top octave sounds as loud as
the middle.

The **E-Bow** drives the string at its fundamental, as its pickup coil hears
it, and holds it at a set level; the more **Force**, the sooner it swells.
**Buzz** is a first-order allpass at the bridge whose angle follows the
string's amplitude (Pierce and Van Duyne's passive nonlinear filter): it
moves energy up the spectrum and takes none, and the string, which loses its
upper partials sooner, ends up quieter than without it. **Bow Lift** is the
time the bow takes to leave the string after note-off: its speed and its grip
both fade over it.

**Decay** is the T60 at the fundamental, independent of pitch;
**Brightness** sets how much sooner the upper partials die; **Material**
tilts that from gut and wood (dark, soft) to metal (bright, stiffer).
**Stiffness** stretches the partials as a stiff string's are,
f_n = n f0 sqrt((1 + B n^2) / (1 + B)), with one B for every engine that grows
with pitch as along a piano (the waveguide's allpasses after Rauhala and
Valimaki, DAFx-06). A bowed string is a tenth as stiff: a bow locks the
partials to one period, and the stretch of a plucked string kept it off the
fundamental (at the default Stiffness, at 48 kHz, the bow sounded around the
octave). **Tension** gives hard plucked and struck notes a pitch
transient that settles to the tuned pitch, and **Detune** sets the second
polarisation (waveguides) or the second course (Struck) apart, for beating
and a two-stage decay.

Note-off damps the string within **Release**; at its maximum notes ring
free, as on a harp or a dulcimer. **Mute** is a palm on the string. The
sustain pedal (CC 64) holds released notes.

The **Sympathetic** strings are a shared bank of up to 12 lossy strings driven
by the played ones (Octaves, Fifths, Major, Minor, Drone, Chromatic, or
Follow, which retunes to the last notes played). The **Body** is a shared
stereo modal resonator (Guitar, Violin, Cello, Harp, Gourd, Soundboard, Box,
Metal); **Coupling** also lets each string lose energy into the body's first
modes at the bridge, so partials near a body resonance decay faster.
**Coupling Detune**, at 1 by default, keeps the fundamental where the coupling
leaves it and lets every other harmonic move: near a body mode, a detuned,
inharmonic pull. At 0, each partial is pulled only by the body's loading where
it is, as on a real instrument; between, the pull is between the two.

Pitch bend, channel pressure, polyphonic aftertouch and CC 74 are read per
MIDI channel, so MPE works without a mode switch. **Pressure** chooses what
pressure drives (Auto: bow speed on Bow, brightness otherwise); CC 74 moves
the excitation point. **Play Mode** Mono re-excites on every note, Legato
slides without re-exciting; **Glide** sets the slide time in both.

Every port is clamped in the DSP, since a preset or an OSC message can hold
any value. Each string is passive by construction (|H| <= 1 for every loop
filter, a bounded bow reflection, a regulated E-Bow); a voice that still
produces a non-finite or runaway value is reset alone, and the output goes
through a limiter placed after **Level**. Nothing allocates after
`prepare()`. With 16 voices sounding, it uses 3.5 to 7.5 % of one core at
48 kHz, sympathetic strings and body included (release build, measured
over the first half second of the notes, when the pitch still moves).

The tuning compensates the exact phase delay of every filter in the loop
where the partials are, weighted as a plucked string's are: Pluck, Struck
and Buzz are within 1.5 cent from 44.1 to 96 kHz over C2-C7, at any
Brightness. A bowed note's pitch also depends on how much sooner its upper
partials die, as a real one's does: exact at Brightness 1, up to 8 cents
sharp at C2 in between.

**Decay** holds at any Brightness: the loss filter is a shelf whose gain at
the fundamental is set first, darkness coming second, so the darkest settings
are less dark on long, high notes than on short or low ones. Its gain at DC
equals its gain at the fundamental, so nothing outlives the note. The loop
reads its delay with 8-point Lagrange interpolation, whose own loss at high
frequencies is taken into account.

### Cordito presets

The default package's `Presets/Cordito` collection holds 59 presets, dry apart
from their body. Levels are matched by measurement.

| Category | Presets |
| --- | --- |
| Plucked | Lantern Thread, Steel and Spruce, Falling Lanterns, Thirteen Bridges, Velvet Courses, Quill and Wire, Taut Skin |
| Bowed | Low Rosin, Near the Bridge, Silk Thread Song, E-Bow Horizon, Deep Ground, Glass Bow |
| Struck | Hammered Lattice, Night Hammers, Copper Rain, Felt and Wood, Clockwork Wire, Tangent Touch |
| Drones | Endless Fifth, Buzzing Lattice, Gourd Bloom, Wheel Air |
| Metallic | Iron Bell Wire, Bridge Cable, Prepared Bolt |
| Experimental | Rust Choir, Feedback Loom, Paper Strings |

A second, less literal batch:

| Category | Presets |
| --- | --- |
| Bass | Thumbed Gut, Fretless Moan, Slapped Gut, Sub Wire |
| Lead | Glass Lead, Buzz Lead, Overbowed |
| Keys | Steel Snowfall, Tacks in the Felt, Choked Pluck, Dream Strings |
| Resonant | Dusk Gourd, Bending Bridges, Bright Courses, Nasal Bow, Silk Under Thumb |
| Ambient | Wind Field, Frozen Lake, Ghost Strings, Breath Bloom |
| Percussive | Gourd Snap, Inside the Frame, Tine Wire, Dry Pinch |
| Experimental | Broken Engine, Sympathetic Storm, Spring Tank Pluck, Moth Wings, Tape Warp Buzz, Underwater Hammers |

Presets store their values by port index: Cordito's ports must only ever be
appended to, and its enumerations (Engine, Exciter, Symp. Tuning, Body,
Play Mode, Pressure) likewise.
