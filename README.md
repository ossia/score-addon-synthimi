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

Cordito is a polyphonic physical model of strings: 16 voices, 61 controls, and
five engines selected by **Engine**:

| Engine | Model | Exciters |
| --- | --- | --- |
| Pluck | Digital waveguide: two delay lines meeting at the excitation point, a loss filter and up to four dispersion allpasses at the bridge | Pick, Finger, Hammer, Burst |
| Struck | Up to 48 partials of a stiff string per course (two courses with Detune), struck by a Hunt-Crossley felt hammer | Hammer, Mallet, Pick, Burst |
| Bow | The same waveguide with a friction junction at the bow point: STK's bow table | Bow, E-Bow |
| Buzz | Pluck with a lossless, amplitude-dependent bridge: the string buzzes against a curved bridge | Pick, Finger, Burst |
| Friction | Bow with a physical friction law: hyperbolic friction, solved exactly, sticking and sliding | Bow, E-Bow |

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

**Friction** bows with a physical friction law (Smith and Woodhouse's hyperbolic
friction, as SuperCollider's DWGBowed solves it): the bow carries the string
until it can no longer hold it, then lets it slide. **Bow Speed** sets the
loudness. **Force** is mapped through Schelleng's window for the bow's speed
and position, so its middle is the regular stick-slip (Helmholtz) motion
wherever the bow is: lower, a light, airy surface sound, or the string
sounding its octave close to the bridge; at the top, a raucous crunch.
**Hardness** is the rosin and the hair: how sharply friction falls as the
string slides, and how much the bow rounds the corner of each slip, from
smooth to gritty. Heavy bowing flattens a note by a few cents, as on a real
string.
**Bow** keeps STK's bow table: Force is its pressure, Hardness its rosin,
and Bow Speed changes the timbre more than the loudness. It is cleaner,
often more than a real bow; Friction is livelier and noisier, and its
Force window is narrower close to the bridge.

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

The header mixes what is heard: **String**, **Exciter**, **Symp. Level**
and **Level**, for everything. String is the played strings; the
sympathetic strings are driven before it, so at 0 they still ring, as the
bridge of a string muted at the pickup still shakes. Exciter is the
excitation heard directly: the click of a pick or a finger, the knock of a
hammer or a mallet, a burst's noise, and the rosin of a bow, as loud as the
bow is fast and pressed. At 1, a click peaks about as high as the string it
plays, at middle C, and the rosin is as loud as the bowed string; an E-Bow
touches nothing and is barely heard. String 0 and Exciter up, with the
sympathetic strings and a body, is the attack and the halo it sets ringing
without the note. Symp. Level goes to 4, 12 dB louder than a real
instrument's, for a halo heard on its own. At String 1 and Exciter 0, the
defaults, Cordito sounds exactly as without them.

Four **LFOs** run in every voice, restarted with each note: a key played
again on a ringing string and each note of Mono too, but not a Legato slide.
Each moves one control (its **Target**), with a **Shape** (sine, triangle,
rising and falling saw, square, random), a **Period** that can follow the
tempo, a **Depth**, and a **Delay**: the LFO stays still for that long after
the note starts, then rises to its depth over half as long again, from the
start of its cycle, as a player's vibrato arrives once the note has spoken.
At depth 1: **Pitch** one semitone either
way (a vibrato), **Level** 12 dB, **Pan** from one side to the other,
**Brightness** half its range, **Position** a quarter of the string, **Bow
Speed** from still to twice as fast, **Force** and **Buzz** half their range.
Position moves a bow along the string; a plucked or struck string takes the
LFO's value at the moment it is played. Several LFOs on one target add up. With every
Target Off, Cordito sounds exactly as without them. Every preset stores the
LFOs, Off where it does not use them: loading a preset sets only the
controls it stores, and one without them would leave the previous preset's
LFOs running.

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
partials die and on the bow's force, as a real one's does: Bow is up to 8
cents sharp at C2 at mid Brightness; Friction, in its regular stick-slip
motion, within 3 cents in half the cases and 8 cents in all, across notes,
bow positions, speeds and forces.

**Decay** holds at any Brightness: the loss filter is a shelf whose gain at
the fundamental is set first, darkness coming second, so the darkest settings
are less dark on long, high notes than on short or low ones. Its gain at DC
equals its gain at the fundamental, so nothing outlives the note. The loop
reads its delay with 8-point Lagrange interpolation, whose own loss at high
frequencies is taken into account.

### Cordito presets

The default package's `Presets/Cordito` collection holds 80 presets, dry apart
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

For the Friction engine, voiced for it:

| Category | Presets |
| --- | --- |
| Friction | Rosin Bloom, Over the Fingerboard, Grain Wire, Pressure Line (channel pressure is the bow's force), Bow Crunch, Halo Strings |

For the LFOs:

| Category | Presets |
| --- | --- |
| Motion | Late Vibrato, Pulse Harp, Drifting Rosin, Rotating Felt, Breathing Wire, Throbbing Bridge, Slow Signal, Scattered Glass, Bow Stutter, Tide Drone |

For String and Exciter: the excitation and the halo, the string low or gone.

| Category | Presets |
| --- | --- |
| Halo | Empty Courses, Under the Lid, Rosin Ghost, Knock and Halo, Pick Forward |

Presets store their values by port index: Cordito's ports must only ever be
appended to, and its enumerations (Engine, Exciter, Symp. Tuning, Body,
Play Mode, Pressure) likewise.

## CZumarzu phase distortion synthesizer

CZumarzu is a phase distortion synthesizer after the Casio CZ series: 16
voices, two lines, each with two alternating waveforms and eight-step DCO
(pitch), DCW (timbre) and DCA (level) envelopes, ring and noise modulation.

Each waveform is a cosine read through a distorted phase, as on the CZ's
uPD933 chip, whose transfer functions the tests compare against (MAME's
model, in `tests/ref/upd933`). Its twelve waves are the CZ's eight (Saw,
Square, Pulse, Double Sine, Saw-Pulse, and the windowed Reso Saw, Reso
Triangle and Reso Trapezoid), three more phase maps in the same spirit
(Double Pulse, Reso Pulse, Reso Double Saw), and VPS (vector phaseshaping),
whose **Formant** moves a peak through the spectrum.

The envelopes keep the CZ's rates: a step's time is its rate times the
distance it moves. Each envelope has **Sustain** and **End** steps and a
**Loop**, which goes back to the first step after the sustain step (or the
end) while the key is held: repeated hits, a filter that keeps moving. The
graph edits each step's level (height) and rate (across its column, fastest
on the left); a drag moves a point by as much as the mouse moves.

Phase distortion aliases as soon as the distortion is strong. **Quality**
picks how it is kept down:

| Quality | How | Latency | Aliasing (worst, note 96) | CPU, 16 voices |
| --- | --- | --- | --- | --- |
| Live | Second-order antiderivative antialiasing | none | -34 dB on the waves that jump | lowest |
| Clean | The same, plus band-limited steps at each jump | 9 samples | -54 dB | 2.8 % |
| Clean 2x | Clean, oversampled twice through a polyphase half-band | 9 samples, plus the half-band's | -72 dB | 4.9 % |
| Clean 4x | Clean, oversampled four times | 9 samples, plus the half-bands' | -88 dB | 9 % |

Waves that do not jump are at -62 to -75 dB in Live and Clean already.

### CZumarzu presets

The default package's `Presets/CZumarzu` collection holds 47 presets. Levels
are matched by measurement; the rhythmic ones loop on a 120 bpm grid while
the key is held.

| Category | Presets |
| --- | --- |
| Bass | Acid Line, Grit Bass, Resonant Bass |
| Bells | Glass Bells, Glass Harmonica |
| Brass | Brass Section, Pressure Brass |
| Keys | Alternating Organ, Bitter Keys, Clav, Digital Piano, Organ, Tine Piano, Toy Piano |
| Lead | Detuned Saws, Hollow Flute, Pressure Lead, Saw-Pulse Lead, Sub Pulse Lead, Sync Lead |
| Pad | Formant Choir, Formant Pad, Hollow Choir, Metal Pad, Slow Morph, Soft Pad, Strings |
| Pluck | Kalimba, Metallic Rain, Wah Pluck |
| Percussion | Pitch Drop Tom |
| Rhythmic | Clave, Dotted Delay, Filter Ticks, Gallop, Laser Hits, Octave Trill, Sixteenths, Stutter, Triplet Echo |
| Textures | Deep Space, Ring Swarm |
| Effects | Broken Radio, Noise Sweep, Zapper |
| Other | Chip Arp, Talking Wire |

## ViceVersa vector synthesizer

ViceVersa is a vector synthesizer after the Prophet VS: 16 voices, four
wavetable sources on the corners of a diamond (A west, B north, C east, D
south), mixed by where a point is in it. The mix follows the VS's: each
corner alone at full level, the centre a quarter of each. **Mix Law** can
also be equal power, for sources that do not add in phase.

The point is the **Position** (one 2D control, to map or automate as one),
plus a five-point mix envelope, the LFOs, and per-note expression (pressure
to Y, CC 74 to X). The envelope moves through its points in four timed
moves (tempo-synced if wanted), can loop over a span of them, round or back
and forth, while the key is held, and goes to its last point on release. On
the diamond, two buttons pick what a drag moves: the envelope's points, or
the Position, as the VS's joystick. The point of each sounding voice is
shown on it.

Each source reads one of 24 band-limited wave banks, at its own frame,
tuning and level:

| Kind | Banks |
| --- | --- |
| Classic | Sine-Saw, Shapes, Square-Saw, Pulse Width, Sync |
| Additive | Partials, Harmonics, Organ, Glass, Comb |
| Digital | Fold, Digital, Phase Dist, FM |
| Formant | Vowels |
| Instruments | Brass, Reeds, Strings, Voices, Keys, Plucked, Winds, Mallets |
| Noise | Noise |

The instrument banks are harmonic spectra resynthesised from the One Laptop
Per Child sound sample library (https://wiki.laptop.org/go/Sound_samples,
CC-BY 3.0), as the VS's own instrument waves were: single cycles through a
note, from its attack to its sustain. `tools/extract_waves.py` generates them.

The **Modulation** tab has four LFOs per voice, as Cordito's. Each moves one
target: X, Y, the pitch of every source or of one, a source's frame, the
filter's cutoff or the level, with its shape, period, depth, delay and the
phase each note starts it at. Two LFOs on X and Y a quarter of a cycle apart
draw a circle (sine) or run around the diamond's edge (triangle).

Then a four-pole ladder low-pass with its envelope, and the amplitude
envelope. Spread places A and B to the left and C and D to the right, per
voice, so that a single note is as wide as a chord.

### ViceVersa presets

The default package's `Presets/ViceVersa` collection holds 36 presets.
Levels are matched by measurement.

| Category | Presets |
| --- | --- |
| Bass | Fold Bass, Fold Growl, Formant Bass, Square Saw Bass, Sub Pulse |
| Bells | Bell Morph, Bell Vector, Digital Bells |
| Brass | PD Brass |
| Keys | Breathing Organ, Organ Drift, Velvet Keys, Wah Keys |
| Lead | Digital Grit, Pulse Rider, Sync Lead, Sync Sweep Lead, Wide Saws |
| Pad | Breathing Pad, Choir of Glass, Harmonic Choir, PD Strings, PWM Strings, Underwater, Vector Pad, Vowel Morph |
| Pluck | Glass Pluck |
| Percussion | Granite |
| Rhythmic | Morph Arp, Octave Cascade, Vowel Sequencer |
| Textures | Evolving Texture, Night Drone, Spectral Drift |
| Effects | Comb Sweep, Glitch Corners |

### Regenerating the presets

The presets of both are generated: after changing their ports or voicing,

    score_addon_synthimi_make_presets <package>/Presets

writes them again, levels matched.
