# SHARD

[![License: GPL-3.0-only](https://img.shields.io/badge/license-GPL--3.0--only-blue.svg)](LICENSE)
[![build-shard](https://github.com/LCFInd/shard/actions/workflows/build-shard.yml/badge.svg?branch=shard)](https://github.com/LCFInd/shard/actions/workflows/build-shard.yml)

SHARD is sample-mangler firmware for the **M-VAVE FM-1**. It cuts a loop into slices, plays them from the step
sequencer, and pushes them through grains, feedback and noise until they break. It's a fork of
[Felucca](https://github.com/hugelton/Felucca), the open-source FM-1 firmware by Leo Kuroshita. Felucca's
sequencer, storage, USB and hardware layer are its foundation.

The synth engines are gone. Three engines remain: **SHARD** (the slice engine), **SAMPLE** and **GRAIN**.

> **Status: untested on hardware.** Every host test passes and the firmware builds with the FM-1's own compiler,
> but SHARD hasn't yet run on a real FM-1. Back up your projects before installing.

## Install

1. Connect the FM-1 by USB.
2. Open **[SHARD's installer](https://lcfind.github.io/shard/)** in Chrome or Edge and press Install. It always has
   the newest build.

Or download the `.fwsc` from the [shard-latest release](https://github.com/LCFInd/shard/releases/tag/shard-latest)
and install it from the command line (needs `pip3 install mido python-rtmidi`):

```
python3 tools/fm1_install.py felucca-shard-XXXXXXX.fwsc
```

Felucca's hosted installer installs only Felucca's own releases, so use one of the two routes above. Both installers
offer a way back: Felucca's [installer](https://hugelton.github.io/Felucca/) puts stock Felucca back on, and its
"Return to official V15" option restores M-VAVE's firmware.

## What it does

- **Slicing.** 2 to 32 equal slices, set by hand, or AUTO. AUTO finds the hits (DETECT HITS), the note changes
  (NOTES), or both. SENS sets how many it finds.
- **Per-slice sound.** Each slice has its own pitch, level, envelope, play mode (ONE, GATE, LOOP), direction
  (forward, reverse, ping-pong), filter, drive, delay send and choke group.
- **PLAY mode.** Plays like ReCycle: the loop follows the tempo and keeps its pitch.
- **GRAIN.** Separates pitch from length, per slice or for the whole loop. STRETCH is a clean time-stretch, CLOUD
  smears the slice, and FREEZE holds its end.
- **Four channels per track.** LOW, MID, HIGH and FULL, each with its own shaper, drive, gate and level, and
  **FDBK**: feedback that rings, then howls on its own like a no-input mixer. **PRESS** lets you lean on a key to
  push the drive and tip a channel into feedback.
- **The ARP layer.** Hold ARP and play keys for a second effects bank: SCATTER (random repeats and reversals),
  FEEDBACK SWELL, RING MOD (three fixed pitches or a sweep that follows the LFO) and NOISE bursts. The four knobs
  set RING, COLOR, CHAOS and PITCH. OCT+ puts RING and NOISE before or after the slice effects, and OCT- is
  **PANIC**: everything off and every howl stopped.
- **LIVE.** The SCL button launches each track's pattern from four saved slots.
- **BOUNCE.** Records the mangled result into a sample slot, ready to slice again.
- **USR4.** A fourth sample slot, up to 7.4 s.
- **The look.** Black and white, square corners and a condensed grotesk.

The full reference, with every page, setting and how each part works, is in **[SHARD.md](SHARD.md)**.

## Build it yourself

You don't need to. Every push to the `shard` branch is built by GitHub with the FM-1's toolchain, then published
to the shard-latest release and the installer.

To build locally, follow [BUILDING.md](BUILDING.md): `./build.sh` on Linux x86-64, or in Docker on macOS. The
mangler build is the default; `FELUCCA_MANGLER=0` builds Felucca's full engine set with SHARD added. The host tests
are in `tests/run_host_tests.sh`.

## Credits and licence

SHARD is built on [Felucca](https://github.com/hugelton/Felucca) by Leo Kuroshita
([@kurogedelic](https://github.com/kurogedelic)), Hügelton Instruments. Felucca's own readme is kept as
[FELUCCA.md](FELUCCA.md). SHARD is an independent modification, and Felucca's maintainer does not test or support it,
so please report SHARD problems [here](https://github.com/LCFInd/shard/issues), not on Felucca's tracker.

SHARD was developed with an AI coding assistant (Claude), from the owner's direction and design.

SHARD is free software under [GPL-3.0-only](LICENSE), the same as Felucca. The Hügelton Sample Pack is CC0 1.0.
Felucca's engines ([Felucca-Engines](https://github.com/hugelton/Felucca-Engines), MIT) and hardware layer
([felucca-bsp](https://github.com/hugelton/felucca-bsp), MPL-2.0) keep their own licences, as does the TeX Gyre
Heros font (GUST Font License). See [CREDITS.md](CREDITS.md) and [LICENSING.md](LICENSING.md).

M-VAVE and FM-1 are trademarks of their respective owners. SHARD is not affiliated with, endorsed by or supported by
M-VAVE.
