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

## LOOP (EDIT's first page on SHARD)

LOOP shows the whole sample with every slice marker. A slice a voice is playing is drawn inverted (white, with its waveform cut out in black) and has its playhead (`shd_heads`). Keys pick the slice, as they do on SLICES.

The track's ENV (ATK, DEC, SUS, REL on KNOB 1–4) is drawn in red over the loop, in the loop's time, starting at the slice the last note began on. It assumes the note is held for one slice; ONE has no release. Red (`ux.env`) is used for nothing else. On SHARD tracks the ENV page is hidden (ENV opens on ENV DEST), and `page_first` returns a family's first visible page.

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

## FDBK (SLICE FX: BUS, per channel)

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
- **PANIC (OCT- on the ARP layer, held or locked):** turns off every ARP effect, latched ones included, and every `arp_k` knob. It also increments `shd_kill`, which each SHARD part notices once per block: it zeroes its loop states and holds FDBK off for 690 blocks (about 0.5 s). OCT+ does nothing on this layer. The shapers' own tails die away in about 50 ms, at about −44 dB.
- **The howl's ceiling:** a channel with FDBK on is compressed 4:1 above 12000 (−9 dBFS), and its output is capped at 20000. The master limiter and soft clip still sit after it.
- **PRESS (BUS, after DETECT; 0 = off):** pressure (`t->at` × PRESS / 127) is added at half strength to every channel's DRV, and to FDBK on channels whose FDBK is already above 0.
  - **Enabling the sensing:** `mangler_trim` sets the track's AFTERTOUCH sensing to 6 while PRESS is on, and keeps AT's own modulation amount at 0.
  - **Where it applies:** played keys F3–C5 only. The host builds and the emulator have no pressure.
- **Storage:** `press` takes one of the bus's spare bytes; the bus stays 32 bytes.
