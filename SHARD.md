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
| **Per-slice settings** | PITCH, LEVEL, an envelope (ATTACK, DECAY, SUS, REL), MODE (track / ONE / GATE / LOOP), DIR (FWD, REV, PING), FILTER (LP / HP / BP / COMB- / COMB+) with CUT and RES, DRIVE (OVRDRV DIST FOLD CRUSH DECIM HALF TUBE FUZZ) with AMT, DELAY, ROUTE, CHOKE (1–4) |
| **Direction** | MODE and DIR together give forward once (ONE + FWD), forward loop (LOOP + FWD), reverse once (ONE + REV), reverse loop (LOOP + REV) and forward-back loop (LOOP + PING). ONE + PING plays forward then back once |
| **Envelope** | ATTACK, then DECAY down to SUS, held while the key is down. REL starts at the key's release. REL OFF leaves the release to the track's envelope. In ONE the slice ignores the key's release |
| **DELAY** | A per-slice send into Felucca's tempo delay, beside the track's own send. It's taken straight from the slice, before the track's DIST and INSERT |
| **PLAY mode** | ReCycle style. A note plays the whole loop from its slice, and each slice starts at its place in the loop, scaled to the tempo. Changing the tempo moves the slices, not the pitch. BARS: AUTO (from the sample's tempo), 1, 2, 4 or 8 |
| **Tempo** | SHARD finds a rhythmic sample's tempo when the slot is loaded (60–200 BPM; a loop cut on the beat gets its exact tempo). You can set it instead on SLICE FX's BUS (BPM, 40–250; AUTO shows the one found). It's kept with the kit |
| **Slices** | DIV: 2, 4, 6, 8, 12, 16 or 32 equal slices, AUTO (one per hit) or MAN (set by hand). SENS on SLICE FX's BUS sets how readily AUTO finds hits in a user sample (0 fewer, 127 more; 64 is SLICE's). The BREAK and PIANO slices are fixed |
| **Four channels** | Ohmnicide style, per track. A slice routes DIRECT to the track, into MULTI (split into the low, mid and high channels), or straight into one channel: LOW, MID, HIGH or FULL. FULL is full-range and starts clean (shaper OFF, level 127). Each channel has its own shaper, drive, gate and level. MULTI (E7) is the bus's wet mix; DRIVE (E8) offsets every channel's drive |
| **USR4** | A fourth sample slot (80 KB, up to 7.4 s), in the flash that FM6's voice bank used, plus the spare area Felucca keeps for a firmware's own data (0xE7000..0xFAFFF). SHARD's SRC lists it after PIANO; SAMPLE's SET and GRAIN's SRC after USR3 |
| **BOUNCE** | SAVE > BOUNCE renders the sequence into a slot: BARS (1, 2, 4, 8), FROM (MIX, or one track), TO (USR1..4), OCT+. The audio goes quiet while it renders, faster than real time, with a progress bar. It plays the bars once unheard, then records them a second time, so delay and reverb tails wrap round and the slot loops seamlessly. Mono, 22.05 kHz, up to 7.0 s (longer bounces are cut). SHARD slices it, and its kit gets the song's tempo as BPM. It won't overwrite a slot that one of the rendered tracks plays |
| **The kit** | A source's per-slice settings and bus settings. A user slot's kit is saved with its sample, in the same sector as SLICE's hand-set slices. The BREAK and PIANO kits last until power-off |

## On the device

- **PRESETS:** SHARD, SAMPLE, GRAIN. SHARD's presets are RECYCLE (PLAY), CHOP KIT (ONE) and SHRED (GATE, 32nds). At power-on tracks 1–3 play SHARD's three presets and track 4 GRAIN.
- **EDIT 1 (SHRD):** SRC, DIV, START, PTCH. This is the same layout as SLICE; DIV adds 2, 6 and 12.
- **EDIT 2 (PLAY):** MODE, BARS, MULTI, DRIVE.
- **SLICES:** set the slices by hand, as with SLICE.
- **SLICE FX:**

  | Control | Does |
  |---|---|
  | KNOB 1 | Picks a slice, or BUS after the last one. A key also picks its slice |
  | KNOB 2 | Picks the parameter |
  | KNOB 3 | Sets the value for the picked slice |
  | KNOB 4 | Moves every slice's value by the same amount |

  On BUS the parameters are BPM, SENS, XLOW, XHIGH, then TYPE, DRV, GATE and LVL for each channel (L, M, H, F). Turning BPM up from AUTO starts at the tempo that was found. SENS re-slices the sample once the knob rests; voices playing it stop.

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
