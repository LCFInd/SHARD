# SHARD on Felucca

SHARD is a slice-loop engine for the M-VAVE FM-1. This branch adds it to
[Felucca](https://github.com/hugelton/Felucca) as engine 15, built on Felucca's SLICE engine. SHARD uses the same
material, slices, decoder and storage as SLICE.

This build is a sample mangler: Felucca's synth engines (ANALOG, PHASE, LOFI, VOICE, TRIO, WHEEL, PHYS, DRUM, NOISE,
FM6) are left out. SHARD, SAMPLE and GRAIN remain. Their engine numbers stay reserved, so stored sounds keep their
places; a sound on a removed engine is silent. `FELUCCA_MANGLER=0` builds Felucca's full set with SHARD added.

Felucca's four tracks are SHARD's four channels, and Felucca's step sequencer drives them. That sequencer already
has ratchets, chance, swing, conditions and parameter locks. SHARD adds the following:

| | |
|---|---|
| **Per-slice settings** | PITCH, LEVEL, an envelope (ATTACK, DECAY, SUS, REL), MODE (track / ONE / GATE / LOOP), DIR (FWD, REV, PING), FILTER (LP / HP / BP / COMB- / COMB+) with CUT and RES, DRIVE (OVRDRV DIST FOLD CRUSH DECIM HALF TUBE FUZZ) with AMT, DELAY, CHOKE (1–4) |
| **Direction** | MODE and DIR together give forward once (ONE + FWD), forward loop (LOOP + FWD), reverse once (ONE + REV), reverse loop (LOOP + REV) and forward-back loop (LOOP + PING). ONE + PING plays forward then back once |
| **Envelope** | ATTACK, then DECAY down to SUS, held while the key is down. REL starts at the key's release. REL OFF leaves the release to the track's envelope. In ONE the slice ignores the key's release |
| **DELAY** | A per-slice send into Felucca's tempo delay, beside the track's own send. It's taken straight from the slice, before the track's DIST and INSERT |
| **PLAY mode** | ReCycle style. A note plays the whole loop from its slice, and each slice starts at its place in the loop, scaled to the tempo. Changing the tempo moves the slices, not the pitch. BARS: AUTO (from the sample's tempo), 1, 2, 4 or 8 |
| **Tempo** | SHARD finds a rhythmic sample's tempo when the slot is loaded (60–200 BPM; a loop cut on the beat gets its exact tempo). You can set it instead on SLICE FX's BUS (BPM, 40–250; AUTO shows the one found). It's kept with the kit |
| **Slices** | DIV: 2, 4, 6, 8, 12, 16 or 32 equal slices, AUTO (one per hit) or MAN (set by hand). SENS on SLICE FX's BUS sets how readily AUTO finds hits in a user sample (0 fewer, 127 more; 64 is SLICE's) |
| **Deck mode** | Tracks 1 and 2 are decks A and B, mixed by a crossfader. Each deck has its own 3-band isolator (LO, MID, HI from KILL to +6 dB, at XLO and XHI), a shaper on each band (TYPE, DRIVE, Ohmnicide style), feedback per band and a DJ filter (FILT). Tracks 3 and 4 play straight into the mix. See DECK MODE below |
| **USR4** | A fourth sample slot (80 KB, up to 7.4 s), in the flash that FM6's voice bank used, plus the spare area Felucca keeps for a firmware's own data (0xE7000..0xFAFFF). SHARD's SRC, SAMPLE's SET and GRAIN's SRC list it after USR3 |
| **BOUNCE** | SAVE > BOUNCE renders the sequence into a slot: BARS (1, 2, 4, 8), FROM (MIX, or one track), TO (USR1..4), OCT+. The audio goes quiet while it renders, faster than real time, with a progress bar. It plays the bars once unheard, then records them a second time, so delay and reverb tails wrap round and the slot loops seamlessly. Mono, 22.05 kHz, up to 7.0 s (longer bounces are cut). SHARD slices it, and its kit gets the song's tempo as BPM. It won't overwrite a slot that one of the rendered tracks plays |
| **The kit** | A source's per-slice settings and bus settings. A user slot's kit is saved with its sample, in the same sector as SLICE's hand-set slices |
| **Samples** | Only your own: the built-in BREAK, PIANO, FLUTE and SAX are gone (`FELUCCA_BUILTIN 0`; the flash they used stays free). A slot with a sample shows the name it was uploaded with, as SRC / SET and as the track's name on the main screen, unless the track plays a user preset |

## On the device

- **PRESETS:** SHARD, SAMPLE, GRAIN. SHARD's presets are RECYCLE (PLAY), CHOP KIT (ONE) and SHRED (GATE, 32nds). At power-on tracks 1–3 play SHARD's three presets and track 4 GRAIN.
- **EDIT 1 (SHRD):** SRC, DIV, START, PTCH. This is the same layout as SLICE; DIV adds 2, 6 and 12.
- **EDIT 2 (PLAY):** MODE, BARS, MULTI, DRIVE. On a deck, MULTI mixes dry and the isolator (127: all through it) and DRIVE adds to the deck's DRIVE.
- **SLICES:** set the slices by hand, as with SLICE.
- **SLICE FX:**

  | Control | Does |
  |---|---|
  | KNOB 1 | Picks a slice, or BUS after the last one. A key also picks its slice |
  | KNOB 2 | Picks the parameter |
  | KNOB 3 | Sets the value for the picked slice |
  | KNOB 4 | Moves every slice's value by the same amount |

  On BUS the parameters are BPM, SENS, DETECT and the track's GRAIN (SIZE, DENS, JITTER). Turning BPM up from AUTO starts at the tempo that was found. SENS re-slices the sample once the knob rests; voices playing it stop. (ROUTE, PRESS, the split points and the four channels moved to the decks: DECK MODE.)

  The kit is saved when you leave the page, once the transport is stopped ("KIT SAVED").

- **SAVE > BOUNCE:** KNOB 1 BARS, KNOB 2 FROM, KNOB 3 TO, OCT+ bounces. The panel shows the length at the current tempo and which slot it replaces, or why it can't run.

## Build

Build it as Felucca: see `BUILDING.md`. On macOS this uses Docker; on Linux x86-64 it runs natively.
`FELUCCA_MANGLER=0` builds Felucca's full engine set (and 3 slots). `FELUCCA_SHARD=0` (with `FELUCCA_MANGLER=0`) builds without SHARD. Engine 14 stays reserved for ADPCM either way.

## Tests

`tests/run_tests.sh` runs `tests/shard_test.c` after the SLICE test. That test checks:

- the engine table
- ONE, GATE and LOOP
- PLAY's slice timing at 120 and 90 BPM, and that pitch doesn't change with tempo
- per-slice LEVEL, PITCH, DECAY and REV, every filter × drive combination, and CHOKE
- that MULTI at mix 0 equals DIRECT bit for bit, and that every route and shaper stays bounded
- the kit's storage beside the hand-set slices
- tempo detection on eight made loops (88–174 BPM, one with a tail that isn't whole beats), the swung test loop and a held tone; the loop length from the tempo; the kit's BPM override
- DIV 2, 6 and 12: exact slice starts and decoder states
- SUS holding and REL ending a slice, PING once (twice the slice, the way back mirrored) and looping, COMB+/COMB- at their delay, FUZZ's crest factor, the per-slice DELAY send, FULL clean against DIRECT, SENS slicing more or less, and the new fields stored with the kit

- the mangler's engine table; USR4's place, scan, tempo and playback; the update-record guard on written sample data
- BOUNCE: the length at a tempo, the slot's header and slices, the kit's BPM, the recording against the live sequence (correlation 0.9996), what stops it (a slot in use, MIX vs one track, external clock), the cut at 7.0 s into USR4

`tests/run_tests.sh` runs Felucca's own suites on the full build (`FELUCCA_MANGLER=0`) and then the mangler's: SHARD's test and the UI renders.

`tests/ui_render.c` renders the SLICE FX page and checks every parameter and value against the layout lint.
`tests/run_host_tests.sh` is `run_tests.sh` without the steps that need a built package.

## Status

All of Felucca's host tests pass with SHARD in, and Felucca's existing golden renders are unchanged. SHARD itself
has **not yet run on an FM-1**. Check these on hardware, in order:

1. CPU load. On the host, SHARD with 8 notes in PLAY costs about the same as ANALOG, roughly 4× SLICE.
2. RAM. The kits take 2.7 KB, the tempo finder 1 KB, the comb filters' lines 8 KB of each SHARD part's engine memory (inside ENG_MEM_PART, which every engine already reserves), and the 192-point slice grid (for 6 and 12 slices) 768 bytes more than SLICE's 128. The engine memory stays within ENG_MEM_PART.
   The tempo finder runs in the main loop when a slot is loaded; time it on a full 7.4 s slot.
3. The kit save on real flash.
4. USR4 (0xE7000..0xFAFFF) on real flash: an upload, a bounce, and that a firmware update leaves it alone.
5. BOUNCE's time: the render runs at the CPU's pace (faster than real time on the host); the audio is silent meanwhile.
6. Flash size. A size proxy (rv32imc, -Os) puts this build about 53 KB under upstream Felucca's, so it should fit the app slot; the real check is the FM-1 toolchain's link.

The web editor bundled in this tree (`web/editor.html`) knows about engine 15. The separate Felucca-WebApp editor
doesn't yet, so it can't edit kits.

## The look: SWISS (the mangler build)

The mangler build has its own black-and-white look:
- **Type:** TeX Gyre Heros Cn, a condensed grotesk (GUST Font License, in `assets/fonts/texgyre`). Labels are regular and values are bold. The faces sit on Inter Tight's line metrics, so every layout keeps its place (`tools/gen_aa_font.py --preset swiss`).
- **Colour:** black, white and four greys, whatever MENU > COLOR is set to (COLOR reads SWISS). `gfx.c palette_set` sets these.
- **Shapes:** every corner is square, the keycaps too (`tools/gen_aa_keycaps.py --swiss`). There are no cards and no dividers, only space between the strips.
- **Parameter icons are off** (`FELUCCA_ICONS` defaults to 0): labels are words. Only the knob you're turning has a white label and gauge; the others are grey.
- The full Felucca build keeps Inter Tight and its palettes.

## LOOP (ENV's first page on SHARD)

LOOP shows the whole sample with every slice marker. A slice a voice is playing is drawn inverted (white, with its waveform cut out in black) and has its playhead (`shd_heads`). Keys pick the slice, as they do on SLICES.

The track's ENV (ATK, DEC, SUS, REL on KNOB 1–4) is drawn in red over the loop, in the loop's time, starting at the slice the last note began on. It assumes the note is held for one slice; ONE has no release. Red (`ux.env`) is used only for this and for the GRAIN engine's scrub line (below). LOOP is in the ENV family, in place of ENV's own page, which is hidden on SHARD tracks: ENV opens on LOOP, then ENV DEST. EDIT opens on EDIT 1. (Until 2026-11-14 LOOP was EDIT's first page.) `page_first` returns a family's first visible page.

## Themes (the editor's Theme editor)

SHARD's look is black and white, whatever COLOR says. A **CUSTOM** theme (Felucca 1.6's user palette, set from the editor) keeps that look's structure but uses its own five colours (`gfx.c` `palette_set`, the mangler branch):

| Editor role | Palette slot | Used for |
|---|---|---|
| Background | color1 | the background, and the ink of a selection |
| Main | color2 | waveforms, values, the active thing, a selection |
| Text | color3 | names, slice lines, key caps |
| Red line, REC | primary | LOOP's envelope, GRAIN's scrub line, REC |
| Accent | secondary | the track number, markers |

The greys (labels, inactive things, rules, wells) are the background mixed toward the text, in the same steps as the black-and-white greys. In the editor's Settings tab, the **Theme editor** has colour pickers, starting themes, a preview of the LOOP screen, readability warnings and a live preview on the FM-1 (PALETTE_SET op 0). Save stores the theme (op 1), Discard drops the preview (op 3), and Remove deletes CUSTOM (op 2). Export and Import use a small JSON file. MENU > DISPLAY > COLOR on the FM-1 switches between CUSTOM and the black and white.

## The SAMPLE engine: START, LEN and PLAY

SAMPLE's second EDIT page is **TRIM** (it was TONE): CUT, **START**, DRV, **LEN**. Its first page's LOOP is now **PLAY**. Between them, a sample can be trimmed to any part and played in either direction, down to a single-cycle loop that plays like a wavetable oscillator for bass and melodies.

- **START** (0–100 %) is where the region begins in the zone. Moving it while a LOOP plays moves the loop (a wavetable scan).
- **LEN** is the region's length from START, on the ENV time scale. Its steps are fine at the short end (1.1 ms upward, for single cycles) and coarse toward the top; 10 s (the top) is the rest of the sample.
- **PLAY**:
  - ONE: a one-shot through the region.
  - SMPL: the sample's own loop, as LOOP ON was. It starts at START, and LEN ends only a sample without a loop.
  - LOOP: loops the region.
  - PONG: plays the region forward, then back, and so on.
  - REV: a reverse one-shot.
  - RLOOP: loops the region in reverse.

The old LOOP OFF / ON are PLAY ONE / SMPL, so saved sounds play as before, bit for bit (the regression goldens are unchanged). That includes the old loop's join, where the last sample before a wrap is the one before the loop start.

How it works (`eng_sample.c` `sx_*`): the samples are IMA ADPCM, which decodes only forward from a known state. Each voice reads its zone through two cached 256-sample segments, decoded from a seek index (the decoder state before every 256th sample of each zone of the SET), as GRAIN does. Any position in either direction is then plain arithmetic, at about one decode per source sample, as before. The index and the caches (11.5 KB) live in the track's engine memory (`engines.c` `eng_mem`), not in RAM's budget. `sample_block` builds the index one entry per block (four while a voice waits for one), and the voices extend it as they decode forward. A START far into a SET loaded a moment ago therefore waits for its entry: about 30 ms at most for a built-in set, longer for a full user slot.

Saves of before had 0 in the unused E5 and E7. START 0 is the start, and LEN's minimum is 1, so a stored 0 is the whole sample: projects (`project.c` `proj_smp_len`) and user presets (`upreset.c` `up_migrate`) are corrected on load, and the engine reads 0 as the whole sample too.

The EDIT pages show START..END tinted, with its waveform bright and the rest dim. Its edges are drawn (with PLAY SMPL, the zone's loop points instead), and a red line marks each sounding voice's read position.

## The GRAIN engine's view (its EDIT pages)

Both GRAIN EDIT pages (GRAN and SPRY) show the waveform of the zone the last note picked, the same view SAMPLE uses (`sample_wave`). On top of it:

- **The scrub line** (red, `ux.env`) marks where the grains centre. That is POS plus the track's SHP modulation, as the engine last used it while a note sounds (`gr_ui_pos`). An LFO or the ENV routed to SHP visibly moves the line. With no note sounding, the line sits at POS.
- **The SPRD range** is tinted round the line, and the waveform is brighter inside it: that is where grains can start.
- **Every grain sounding** in that zone is a white tick at its read position. Its height is the grain's window at that moment, so ticks swell and fade as the grains play.

The waveform loads at 4096 samples a frame (it was 512), so a full user slot fills in within about 40 frames. A built-in set is no longer redecoded after an upload into a user slot.

## LIVE (the SCL button)

In the mangler build, SCL opens LIVE instead of scales and chords. LIVE launches each track's pattern from the four PROJECT slots (A–D): the 16 patterns a song draws from.

- **Keys:** on the LIVE page, or with SCL held, the 16 white keys are a 4 × 4 grid (a row of four per track, A–D). Black keys 1–4 stop T1–T4; black keys 5–8 return them to their own steps (OWN). On LIVE the keys never sound.
- **Launch timing:** a launch waits for the end of the track's loop (LAUNCH LOOP) or plays from step 1 in the next block (LAUNCH NOW).
- **OCT+ (OWN):** every track back to its own steps; stopped, LIVE turns off.
- **How it works:** LIVE runs as an endless one-section song (`chain.live`, `chain.want`, `live_due` in seq.c's step loop, `CHAIN_OWN` lanes). PLAY with LIVE on loads the saved slots (`live_load`). STOP keeps the lanes, and a SONG started from GLO > SONG ends LIVE.

## GRAIN (SLICE FX: each slice's, or the track's on BUS)

GRAIN plays a slice as grains so that its pitch and its length are separate. A head moves through the slice at the material's own speed. In PLAY it moves at whatever speed makes the slice fill its steps at the song's tempo. Every hop, a grain starts at the head and reads at the pitch rate (PTCH, the slice's PITCH and modulation). Changing the pitch therefore leaves the length alone, and changing the tempo leaves the pitch alone.

- **Modes:** OFF (varispeed, as before) / STRETCH / CLOUD / FREEZE. A slice set to TRACK (the default) follows BUS > GRAIN, so one setting can apply to the whole loop.
  - **STRETCH:** clean pitch and time.
  - **CLOUD:** random start points across the slice (as far as JITTER reaches), random hops and up to ±1 semitone of random detune.
  - **FREEZE:** the slice plays, then its end keeps sounding: while the key is held (GATE, LOOP), for as long again as the slice (ONE), or until the next slice (PLAY).
- **SIZE:** 4–256 ms per grain (50 ms by default). A short slice gets shorter grains, so no grain reads past the slice's end.
- **DENS:** 1–4 grains overlapping (2 by default; at 2 the windows sum flat).
- **JITTER:** how far grains may start behind the head (CLOUD: around it).
- **The attack:** a slice's first grains start on its first sample at the peak of their windows, so the transient is kept. At PTCH 0 with no jitter, STRETCH gives back the slice itself (correlation 0.998 in shard_test).
- **Held tones:** STRETCH and FREEZE move each new grain's start (up to ±64 source samples) to where it best matches the grain already sounding (WSOLA, normalised correlation; the nominal start wins ties). Without this, a tone shifted with grains comes out detuned by the phase slip between grains.
- **DIR:** REV moves the head backwards and PING turns it; the grains themselves always read forwards.
- **Cost:** at most 4 grains per voice and 8 per part. Grain starts are found from the nearest saved decoder state: the head's, one kept every 128 samples behind it, the slice's start, or SLICE's grid. Seeking has a budget of 384 decoded samples per block, saved up to 2048. A grain that does not fit the budget is left out, or started without alignment. On the host, 8 voices of STRETCH cost about 1.5× SHARD without grains, comparable to the GRAIN engine's worst case.
- **Storage:** the kit record is now SHF3 (each slice 20 bytes; the bus's 4 spare bytes hold the track's GRAIN). SHF2 kits load with GRAIN set to TRACK and the bus set to OFF.
- **The GRAIN engine stays** for synth-style granular work on SAMPLE zones.

## DECK MODE (2026-11-17)

Tracks 1 and 2 are decks A and B. Each deck runs through its own isolator after its engine (any engine: SHARD, GRAIN or SAMPLE), then the crossfader mixes them. Tracks 3 and 4 play straight into the mix, as DIRECT did.

| Page | Where | KNOB 1 | KNOB 2 | KNOB 3 | KNOB 4 |
|---|---|---|---|---|---|
| MIX | HOME's first page (decks only) | XFADE | A LVL | B LVL | CURVE |
| ISOLATE | EDIT, after EDIT 2 | LO | MID | HI | FILT |
| XOVER | EDIT | XLO | XHI | PITCH | PRESS |
| LOW | EDIT | TYPE | DRIVE | FB LO | LO |
| MID | EDIT | TYPE | DRIVE | FBMID | MID |
| HIGH | EDIT | TYPE | DRIVE | FB HI | HI |

- **The split:** 12 dB/octave Linkwitz-Riley crossovers at XLO and XHI (40 Hz to 12 kHz), built from trapezoidal one-poles. The three bands add back up flat (only the phase turns). At KILL a band is gone 12 dB/octave beyond its crossover: about −24 dB an octave and a half below XLO or above XHI, about −13 dB in the middle of MID with the default crossovers.
- **LO, MID, HI:** KILL, then −40 dB to 0 dB at the centre (64), up to +6 dB.
- **TYPE, DRIVE (LOW, MID, HIGH):** each band's own shaper, any of SHARD's, before its level. TYPE OFF: none. EDIT 2's DRIVE (on a SHARD deck) and PRESS add to every band's DRIVE.
- **FILT:** left of OFF a low-pass closing, right a high-pass opening (SHARD's state-variable filter at 2×, a little resonance).
- **FDBK (LOW, MID, HIGH):** each band's output band-passed at its own pitch (150 Hz, 900 Hz, 4 kHz; PITCH moves them ±24 semitones, ARP's PITCH too) and fed back, held by its shaper (the shaper's gain undone in the loop, as on the old bus) or, with TYPE OFF, a soft clip. From about 64 a band holds itself after the slices stop. ARP's FDBK SWELL, PRESS and PANIC work on it as they did on the bus.
- **The crossfader:** XFADE −100 % is all deck A, +100 % all deck B. CURVE: SMOOTH (both at full in the middle), DJ (equal power: −3 dB in the middle), CUT (a scratch curve: full until the last sixteenth of the travel). It sets each deck's level everywhere: its dry, its sends, the slices' DELAY sends and the FX layer's THROW.
- **Untouched** (every band 0 dB, TYPE OFF, FILT OFF, no feedback) a deck is its input bit for bit and costs nothing.
- **Where the values live:** the 20 FM operator values every track stores (`core.h` `P_DK_*`), which no engine of the mangler reads. Projects, user presets, the editor and the motion recorder carry them. A sound load keeps them: they belong to the deck, not the sound. XFADE and CURVE are track 1's.
- **Old projects:** a project from before deck mode has the FM defaults there. At its load every track gets the deck defaults, and a deck on SHARD takes its sample kit's crossovers, FDBK and PRESS, and, when its slices went through the bus, the low, mid and high channels' shapers. The gates, levels and FULL are not carried over.
- **Cost (host, instructions per sample at the worst moment, the owner's project with track 2's key hits):** all slices DIRECT 1,960; the old bus with every slice on MULTI 3,040; deck mode with both decks untouched 1,925, both isolators on 2,270, both with feedback and the filter too 2,580, and both with a TUBE shaper on every band as well 2,960 (each band's shaper is the expensive part, as on the old bus).
- **The pictures:** ISOLATE shows a 20-band analyzer (the input as ghost bars with falling peaks, the output on them, a killed band's bars red) with the crossovers as red lines; XOVER draws them solid. MIX shows both decks' meters and the crossfader. LOW, MID and HIGH show each band's FDBK with how loud it rings now (red while it holds itself).

## Your samples only (2026-11-19)

The mangler build has no built-in samples (core.h `FELUCCA_BUILTIN`, 1 in the full build). SHARD's SRC, SAMPLE's SET and GRAIN's SRC offer USR1..USR4 only; the knobs skip the old values. A stored BREAK, PIANO, FLUTE or SAX (projects, presets, the editor) plays USR1. A user slot with a sample is named by the name it was uploaded with (upper case, 8 characters) on the knob, on the track's name in the header (unless the track plays a user preset, whose name wins) and in the editor's menus; an empty slot keeps USR1..USR4. The factory presets start on USR1. The built-in samples' 123 KB of flash are left free.

ISOLATE and XOVER draw each band's LEVEL as a red line across its part of the analyzer: KILL on the floor, then -40 dB up to +6 dB at the top (0 dB at 87 % of the height).

The EDIT menu of a SAMPLE, GRAIN or SHARD track no longer shows FM's OP1..OP4 ENV and OP LEVEL pages: DECK MODE's values share those parameter slots, and the pages were let through on every track. Only the deck pages (ISOLATE, XOVER, LOW, MID, HIGH, MIX) show, on tracks 1 and 2. `tests/ui_render.c` fails on any OP page in the mangler.

## Brighter samples: 4-point interpolation and the upload filter (2026-11-19)

A sample is stored at 22,050 Hz, so it holds nothing above 11 kHz. Below that, two things used to take the top off: the
editor's conversion (a 2-sample average: -2.4 dB at 10 kHz, and 12 kHz folded back to 10 kHz nearly untouched) and the
device's linear interpolation (-6.3 dB at 10 kHz): -8.7 dB at 10 kHz, -5.4 dB at 8 kHz.

- SAMPLE and SHARD play with 4-point (Catmull-Rom) interpolation (`eng_sample.c smp_cr4`, `SMP_CR4`, the mangler only):
  10 kHz -4.4 dB. It costs about 5 % more on a busy project (peak 1,743 -> 1,831 host instructions a sample).
  GRAIN stays linear (its many grains would multiply that); with the upload filter its 10 kHz is about -1.9 dB.
- The editor (`resampleHq`) and `tools/fm1_sample_upload.py` (`sampleio.resample_hq`, the same bytes) convert with a
  Kaiser-windowed sinc (80 dB: 12 kHz now lands 96 dB down), then an 11-tap filter that makes up what the 4-point
  interpolation takes off. On the device: flat within -0.2 / +0.1 dB to 9.9 kHz. The droop of an interpolator is fixed
  in the sample's own frequencies, so this holds at any pitch.
- Samples already in the slots keep the old conversion: upload them again to get the new top end. BOUNCE renders on
  the device without the make-up filter.

## MOD ENV and the MOD matrix (2026-11-19)

Each track has a second envelope, the MOD ENV (ENV > MOD ENV: ATK DEC SUS REL, on the same time scale as the AMP ENV,
drawn the same way). It is a source of the MOD matrix, MENV: per voice like ENV (each note starts it from 0, the key's
release starts its REL), 0..1, and it never ends a voice. Route it on the MOD page (LFO > MOD: four slots of SRC, DST,
AMT) to PITCH, CUT, AMP, the engine's E1..E8 (SAMPLE's START is E6), the sends, and three new destinations: XLO and XHI
(a deck's crossovers) and DRIVE (a deck's three band DRIVEs together). AMT 64 at a full envelope moves a per-block
destination over its whole range; on tracks 3 and 4 the deck destinations do nothing.

The mangler had the MOD matrix switched off (its page hidden, every AMT zeroed on each frame: `ui_input.c
mangler_trim`). It is back: the page shows, and slots keep their AMT. ARP and INSERT stay off. The AMP ENV page (the
track's ADSR, as before) is titled AMP ENV in the mangler. SHARD shows MOD ENV next to its LOOP page.

The values live in every track's DRUM lane levels 5..8 (unused in the mangler; core.h `P_MENV_*`); a project of
before holds 127 in all four and loads the defaults (ATK 0, DEC 60, SUS 0, REL 40: a decaying blip). Cost: the
envelope's tick, one per voice per block; the matrix costs nothing while no slot is active.

## STRUM (2026-11-19)

VOICE's VCE has a fifth mode, STRUM (after UNI; the mangler only): MONO with a power chord. Each note plays three
voices, the key, a fifth (+7) and an octave (+12) above it, each at about 60 % of the velocity so the chord is about
as loud as one note. It behaves as MONO: one chord at a time, a new key retriggers all three (gliding with GLIDE),
letting go falls back to a held key (PRIO), and the last key let go releases the chord. DETUNE does nothing here;
SPREAD puts the fifth on the other side. A voice past note 127 drops an octave. It takes three of the eight shared
voices, and under overload the fifth and the octave go first. The three start together; a strummed delay between
them is not there yet.

### A user slot played silence (fixed 2026-11-19)

Without the built-in samples (1d1e949 on), the user slots' data was addressed from a 4-byte array of zeros. gcc and
clang may read any byte of an all-zero constant array as 0, and both do at -O2 (and likely at -Os, the device's):
SAMPLE, SHARD and GRAIN could play silence from every user slot, in the emulator too. The base is now read through a
pointer the compiler cannot see into (`eng_sample.c smp_data_vp`). `tests/deck_test.c` 9 uploads white noise with
`fm1_sample_upload.py`, plays it on SAMPLE through the mix and the master as the device builds it, and checks it is
heard and flat from 40 Hz to 8 kHz (within 0.9 dB: nothing in the chain cuts the bass).

What a spectrum shows of a white-noise sample: flat to about 9.9 kHz, a notch at 11 kHz (half the stored rate: the
upload filter leaves nothing just below it, so nothing mirrors just above it), and above 11 kHz the interpolation's
images: mirrored copies of 9 .. 11 kHz (about -8 dB at 12.5 kHz, -20 dB at 16 kHz), not real highs.

## MASTER EQ (2026-11-19)

The EQ page (after MIX; on every track) is a 3-band EQ at the end of the mix: LOW (a shelf at 150 Hz), MID (a bell,
Q 0.9, at MID F: 40 Hz .. 12 kHz) and HIGH (a shelf at 5 kHz), -12 .. +12 dB each, in a row after the DC blocker and
before LOWCUT / BASS+ and the limiter (`fx.c meq`). A cut is the exact inverse of the same boost. The page draws the
curve. At 0 dB the EQ is out of the path; on, it costs about 8 % (peak +148 host instructions a sample). It is the
project's (track 1's, like XFADE): stored in track 1's DRUM lane levels, which no engine of the mangler reads. A project
of before holds the lanes' 127 there and loads flat (`project.c proj_deck`). Sound loads keep it. BOUNCE FROM MIX
takes it.

## Overload (2026-11-17)

When the audio runs late (two halves in a row over 85 %), a voice is dropped: first the quietest released voice older than about 46 ms, then a held extra note, and a just-started released voice only when nothing else is left. The header's track icon turns red for 0.7 s each time.

## Complete backups (2026-11-17)

The editor's complete backup holds 16 objects: ids 10–12 are each user slot's SLICE FX kit and MAN slices. A restore writes them after the samples, onto the sample they were made for only. Older backups (13 objects) still restore; older firmware skips the new ids.

## FDBK (before DECK MODE: SLICE FX's BUS, per channel)

In the mangler this is now each deck's LOW, MID and HIGH pages (DECK MODE). What follows describes the bus as Felucca's full build keeps it.

Each BUS channel (LOW, MID, HIGH, FULL) has an FDBK setting, listed between DRV and GATE. It feeds the channel's shaper output back into its own input, the way a no-input mixer is played.

- **The loop's pitch:** the feedback passes through a band-pass (Q 2) at a pitch set by the channel: LOW 150 Hz, MID 900 Hz, HIGH 4 kHz, FULL 500 Hz.
- **Same onset for every shaper:** the loop's gain is divided by the shaper's own gain, which `fai` / `fao` measure as mean input and mean output. A RAT at full drive therefore starts howling at about the same setting as a clean channel.
- **Settings:** about 0–60 thickens the sound and rings. From about 64–80 (depending on type and drive) the loop sustains itself after the slices stop, held at the shaper's ceiling. GATE does not close on the howl, because the follower hears the loop.
- **When the bus stops:** the bus keeps running while the band-passed loop is above −36 dBFS (`hot`). Below that, the gain estimate is clamped (`fao` ≥ 512), so the loop dies away in quiet instead of humming.
- **Storage:** FDBK is 4 bytes at the end of the bus, so the bus is now 28 bytes (SHF3, still unreleased). SHF2 kits load with FDBK 0.

## The ARP layer: a second effects bank (the mangler)

With no ARP pages, the ARP button is now a hold layer like FX: hold ARP and press keys. It uses FX's engine (`perform.c`), knobs (FILTER, CRUSH, THROW, DEPTH), latch, double-tap lock and black-key mutes. The white keys play `PF_ARPMAP`:

| Row | F / C / G | G / D / A | A / E / B | B / F / C |
|---|---|---|---|---|
| 1 (F3–B3) | SCATTER 1/16 | SCATTER 1/32 | FDBK SWELL | – |
| 2 (C4–F4) | RING LO 60 Hz | RING MID 420 Hz | RING HI 1.7 kHz | RING SWEEP |
| 3 (G4–C5) | NOISE 1/8 | NOISE 1/16 | NOISE 1/32 | NOISE HOLD |

- **SCATTER:** records the mix into the loop buffer continuously. At each 1/16 (or 1/32) it picks one of these:
  - Live, a third of the time.
  - One of the last few divisions again (as many as the 371 ms buffer holds beside the one recording). A quarter of these play backwards.
  - It starts on the 1/16. It is a buffer effect: the last-pressed rule applies, the same as for REPEAT and TAPE.
- **FDBK SWELL:** raises `shd_swell`, a floor under every SHARD track's FDBK. It rises over a bar while held and falls over a beat when let go. While swelling, DIRECT slices also feed the FULL channel in proportion, so the default kit howls too.
- **RING:** multiplies the mix by a sine. SWEEP glides 40 Hz–2.4 kHz and back over a bar, in time with the transport.
- **NOISE:** white noise in squared-decay bursts on the 1/8, 1/16 or 1/32 grid; HOLD is steady.
- **DEPTH:** RING and NOISE follow KNOB 4. At 0 % the output is the input, bit for bit.
- **The FX map:** in the mangler, FX's key map can take any of these effects too (PRESETS while holding a key). In Felucca's full build, the list stops at ECHO THROW and there is no ARP layer.
- **A fix along the way:** `pb_at` now wraps its neighbouring frame round the loop. Before, a read straddling the buffer's end took one sample from outside it. Only SCATTER reads across the wrap.

## DETECT (SLICE FX: BUS, after SENS): what AUTO slices at

AUTO slicing now has a DETECT setting with three options, HITS, NOTES and BOTH. BOTH is the default in SHARD; Felucca's own SLICE engine keeps HITS. HITS is pass 2 as before. NOTES is `slc_scan`'s pass 3 (`slc_notes`), and BOTH merges the two passes.

- **Pitch tracking:** the material is decimated to about 5.5 kHz (the mean of `dec` samples). Every 64 decimated samples (about 11.6 ms) YIN runs over a 192-sample window: the first dip of the cumulative-mean-normalised difference under 0.2, searched between 1.1 kHz and 46 Hz, with a parabolic lag. No dip means the frame is unpitched. The frame's level is the peak over the 64 samples at the window's middle.
- **A note change at frame c:** the 6 frames before and after it must all be pitched and steady within 0.35 semitone. Frames c±1, which straddle the change, are left out. The two means must be at least a semitone apart.
  - The step scores min(1, semitones / 5).
  - The dip is the lowest level within ±6 frames, below the lower of the highs either side, as a fraction of that high.
  - With a dip of 8 % or more, the score is (step + dip) × 0.8, the crossover. Otherwise it is step × 0.5.
  - It becomes a slice when the score is at least 0.8 − 0.6 × SENS / 127. Frames next to each other give one slice, the best scoring.
- **Placement:** at the bottom of the dip (or at the step if there is no dip), then moved forward to the next rising zero crossing, at most 12.5 ms on.
- **Merging (`slc_merge`):** in BOTH, a hit within 50 ms of a note slice replaces the note slice, because a hit's attack position is exact.
- **Cost:** on the host, BOTH takes about 5 times as long as HITS (the pitch window read every 2nd sample), and this happens only when a sample loads or SENS or DETECT changes. My estimate is about 0.4 s on the device for a full slot; that hasn't been measured on hardware. A note change shorter than about 70 ms (6 frames) is not found.
- **Storage:** DETECT is 1 byte in the bus, so the bus is now 32 bytes (SHF3). SHF2 kits load with DETECT set to HITS.

## Playing it: ARP's knobs, PANIC, the howl's ceiling, PRESS

- **ARP's knobs:** while ARP is held, KNOB 1–4 are `arp_k` (perform.c), not FX's macros. They reset when ARP is let go, unless LATCH is on.
  - **RING:** ×2^(±2 octaves) on the carrier, SWEEP included; clamped to 8 Hz–9 kHz.
  - **COLOR:** NOISE through a one-pole filter: the low-pass output (darker) or what the low-pass removes (thinner).
  - **CHAOS:** SCATTER's live share goes from 1/3 at the centre to 1 (left) or 0 (right); the backwards share from 1/4 to 5/8 (right).
  - **PITCH:** `shd_fbpitch`, which moves every FDBK loop's band-pass ±2 octaves. The Chamberlin f is capped at 1.4.
  - **Full level:** RING and NOISE play at full level on the ARP layer (`perf_arp`); FX's DEPTH only applies to them on FX's keys.
- **PANIC (OCT- on the ARP layer, held or locked):** turns off every ARP effect, latched ones included, and every `arp_k` knob. It also increments `shd_kill`, which each SHARD part notices once per block: it zeroes its loop states and holds FDBK off for 690 blocks (about 0.5 s). (OCT+ switches PRE and POST: see below.) The shapers' own tails die away in about 50 ms, at about −44 dB.
- **The howl's ceiling:** a channel with FDBK on is compressed 4:1 above 12000 (−9 dBFS), and its output is capped at 20000. The master limiter and soft clip still sit after it.
- **PRESS (BUS, after DETECT; 0 = off):** pressure (`t->at` × PRESS / 127) is added at half strength to every channel's DRV, and to FDBK on channels whose FDBK is already above 0.
  - **Enabling the sensing:** `mangler_trim` sets the track's AFTERTOUCH sensing to 6 while PRESS is on, and keeps AT's own modulation amount at 0.
  - **Where it applies:** played keys F3–C5 only. The host builds and the emulator have no pressure.
- **Storage:** `press` takes one of the bus's spare bytes; the bus stays 32 bytes.

## RING SWEEP follows the LFO; RING and NOISE PRE or POST (ARP's OCT+)

- **RING SWEEP:** the sweep now follows the selected track's LFO instead of a fixed bar-long triangle, so everything on the LFO pages applies: RATE or SYNC (tempo divisions), WAVE, PHASE, TRIG and POL.
  - **POL BI:** the LFO's −1…+1 sweeps 40 Hz to 2.4 kHz (logarithmic).
  - **POL UNI:** its 0…+1 covers that same range.
  - ARP's RING knob still transposes the whole sweep.
- **OCT+ on the ARP layer** switches where RING and NOISE go.
  - **POST (the default):** on the mix, after the master level, as before.
  - **PRE:** inside SHARD's voices. Each slice goes through RING and NOISE right after its sample is read, and only then through its own FILTER, DRIVE, envelope and route, so the bus shapers and FDBK get it too.
  - **The screen:** the header reads "[ARP] PRE" or "[ARP] POST", and the footer shows OCT+ PRE / POST and OCT- PANIC.
- **How PRE works:** `perf_begin` runs before the parts. It fills `shd_pre_c`, `shd_pre_w` and `shd_pre_nz` for the block (up to 64 samples), and SHARD's render loop applies them per sample.
- **What PRE reaches:** only SHARD tracks take RING and NOISE in PRE; SAMPLE and GRAIN tracks don't. NOISE is added per voice, so it follows each slice's envelope and route, and grows with the number of voices sounding.
